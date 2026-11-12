#include <efsw/FileSystem.hpp>
#include <efsw/FileWatcherGeneric.hpp>
#include <efsw/Lock.hpp>
#include <efsw/System.hpp>

namespace efsw {

FileWatcherGeneric::FileWatcherGeneric( FileWatcher* parent ) : FileWatcherGeneric( parent, 1000 ) {
	mInitOK = true;
	mIsGeneric = true;
}

FileWatcherGeneric::FileWatcherGeneric( FileWatcher* parent, unsigned int pollingFreq ) :
	FileWatcherImpl( parent ), mThread( NULL ), mLastWatchID( 0 ), mPollingFreq( pollingFreq ) {
	mInitOK = true;
	mIsGeneric = true;
}

FileWatcherGeneric::~FileWatcherGeneric() {
	mInitOK = false;

	efSAFE_DELETE( mThread );

	/// Delete the watches
	WatchList::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); ++it ) {
		efSAFE_DELETE( ( *it ) );
	}
}

WatchID FileWatcherGeneric::addWatch( const std::string& directory, FileWatchListener* watcher,
									  bool recursive, const std::vector<WatcherOption>& options ) {
	std::string dir( directory );

	FileSystem::dirAddSlashAtEnd( dir );

	FileInfo fi( dir );

	if ( !fi.isDirectory() ) {
		return Errors::Log::createLastError( Errors::FileNotFound, dir );
	} else if ( !fi.isReadable() ) {
		return Errors::Log::createLastError( Errors::FileNotReadable, dir );
	}

	if ( pathInWatches( dir ) ) {
		return Errors::Log::createLastError( Errors::FileRepeated, dir );
	}

	std::string curPath;
	std::string link( FileSystem::getLinkRealPath( dir, curPath ) );

	if ( "" != link ) {
		if ( pathInWatches( link ) ) {
			return Errors::Log::createLastError( Errors::FileRepeated, dir );
		} else if ( !linkAllowed( curPath, link ) ) {
			return Errors::Log::createLastError( Errors::FileOutOfScope, dir );
		} else {
			dir = link;
		}
	}

	WatchID id;
	{
		Lock lock( mWatchesLock );
		id = ++mLastWatchID;
	}

	// Built without the lock, which would pause the polling of every other watch while the
	// directory tree is read; its lookups of the other watches take the lock themselves. It
	// is registered once complete, so the polling thread never sees it half built.
	bool reportCrossDirectoryMoves =
		getOptionValue( options, Options::ReportCrossDirectoryMoves, 0 ) != 0;
	WatcherGeneric* pWatch =
		new WatcherGeneric( id, dir, watcher, this, recursive, reportCrossDirectoryMoves );
	// Watches that opted in share one batch, so a move between two of them is paired.
	if ( reportCrossDirectoryMoves && FileInfo::inodeSupported() )
		pWatch->SharedActionBatch = &mSharedActions;

	Lock lock( mWatchesLock );
	// Another thread may have registered the directory meanwhile.
	if ( pathInWatches( dir ) ) {
		delete pWatch;
		return Errors::Log::createLastError( Errors::FileRepeated, dir );
	}
	mWatches.push_back( pWatch );

	return id;
}

void FileWatcherGeneric::removeWatch( const std::string& directory ) {
	std::string dir( directory );
	FileSystem::dirAddSlashAtEnd( dir );

	Lock lock( mWatchesLock );
	for ( WatchList::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
		if ( ( *it )->Directory == dir ) {
			eraseWatchLocked( it );
			return;
		}
	}
}

void FileWatcherGeneric::removeWatch( WatchID watchid ) {
	Lock lock( mWatchesLock );
	for ( WatchList::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
		if ( ( *it )->ID == watchid ) {
			eraseWatchLocked( it );
			return;
		}
	}
}

void FileWatcherGeneric::eraseWatchLocked( WatchList::iterator it ) {
	WatcherGeneric* watch = *it;
	mWatches.erase( it );
	mSharedActions.forget( watch );
	mRetiring.release( watch );
}

void FileWatcherGeneric::watch() {
	if ( NULL == mThread ) {
		mThread = new Thread( [this] { run(); } );
		mThread->launch();
	}
}

void FileWatcherGeneric::run() {
	do {
		{
			Lock lock( mWatchesLock );

			// Events of opted-in watches are collected during the scans, and dispatched after.
			std::vector<Watcher*>& scanned = mRetiring.beginPass();
			scanned.assign( mWatches.begin(), mWatches.end() );
			mSharedActions.beginPass();
			for ( Watcher* watch : scanned ) {
				if ( watch->Retired )
					continue;
				watch->watch();
				mSharedActions.scanned( watch );
			}
			mSharedActions.finishPass();
			mRetiring.endPass();
		}

		if ( mInitOK )
			System::sleep( mPollingFreq );
	} while ( mInitOK );
}

void FileWatcherGeneric::handleAction( Watcher*, const std::string&, unsigned long,
									   const std::string& ) {
	/// Not used
}

std::vector<std::string> FileWatcherGeneric::directories() {
	std::vector<std::string> dirs;

	Lock lock( mWatchesLock );

	dirs.reserve( mWatches.size() );

	WatchList::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); ++it ) {
		dirs.push_back( ( *it )->Directory );
	}

	return dirs;
}

bool FileWatcherGeneric::pathInWatches( const std::string& path ) {
	// The polling thread changes the directory trees of the watches while it scans them.
	Lock lock( mWatchesLock );
	WatchList::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); ++it ) {
		if ( ( *it )->Directory == path || ( *it )->pathInWatches( path ) ) {
			return true;
		}
	}

	return false;
}

} // namespace efsw
