#include <efsw/FileWatcherKqueue.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE || EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <dirent.h>
#include <efsw/Debug.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/Lock.hpp>
#include <efsw/System.hpp>
#include <efsw/WatcherGeneric.hpp>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace efsw {

FileWatcherKqueue::FileWatcherKqueue( FileWatcher* parent, unsigned int pollingFreq ) :
	FileWatcherImpl( parent ),
	mLastWatchID( 0 ),
	mThread( NULL ),
	mFileDescriptorCount( 1 ),
	mPollingFreq( pollingFreq ) {
	mTimeOut.tv_sec = 0;
	mTimeOut.tv_nsec = 0;
	mInitOK = true;
}

FileWatcherKqueue::~FileWatcherKqueue() {
	mInitOK = false;
	// The worker and its callbacks must finish before their watch objects are freed.
	efSAFE_DELETE( mThread );

	WatchMap::iterator iter = mWatches.begin();

	for ( ; iter != mWatches.end(); ++iter ) {
		efSAFE_DELETE( iter->second );
	}

	mWatches.clear();
}

WatchID FileWatcherKqueue::addWatch( const std::string& directory, FileWatchListener* watcher,
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
		return Errors::Log::createLastError( Errors::FileRepeated, directory );
	}

	std::string curPath;
	std::string link( FileSystem::getLinkRealPath( dir, curPath ) );

	if ( "" != link ) {
		if ( pathInWatches( link ) ) {
			return Errors::Log::createLastError( Errors::FileRepeated, directory );
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

	// The watch is built without the lock, which would pause the polling of every other watch
	// while addAll() opens a descriptor per file; its lookups of the other watches take the
	// lock themselves. It is registered once complete, so the polling thread never polls it
	// half built.
	/// Check first if are enough file descriptors available to create another kqueue watcher,
	/// otherwise it creates a generic watcher
	bool reportCrossDirectoryMoves =
		getOptionValue( options, Options::ReportCrossDirectoryMoves, 0 ) != 0;
	Watcher* added = NULL;
	if ( availablesFD() ) {
		WatcherKqueue* watch = new WatcherKqueue( id, dir, watcher, recursive, this, NULL,
												  reportCrossDirectoryMoves );
		watch->SharedActionBatch = sharedActionsFor( reportCrossDirectoryMoves );

		watch->addAll();

		// if failed to open the directory... erase the watcher
		if ( !watch->initOK() ) {
			int le = watch->lastErrno();

			efSAFE_DELETE( watch );

			// Probably the folder has too many files, create a generic watcher
			if ( EACCES != le ) {
				WatcherGeneric* genericWatch = new WatcherGeneric(
					id, dir, watcher, this, recursive, reportCrossDirectoryMoves );
				genericWatch->SharedActionBatch = sharedActionsFor( reportCrossDirectoryMoves );
				added = genericWatch;
			} else {
				return Errors::Log::createLastError( Errors::Unspecified, link );
			}
		} else {
			added = watch;
		}
	} else {
		efDEBUG( "Using a generic watcher, file descriptor limit reached: %ld\n",
				 mFileDescriptorCount.load() );

		WatcherGeneric* watch = new WatcherGeneric( id, dir, watcher, this, recursive,
													reportCrossDirectoryMoves );
		watch->SharedActionBatch = sharedActionsFor( reportCrossDirectoryMoves );
		added = watch;
	}

	Lock lock( mWatchesLock );
	// Another thread may have registered the directory meanwhile.
	if ( pathInWatches( added->Directory ) ) {
		delete added;
		return Errors::Log::createLastError( Errors::FileRepeated, directory );
	}
	mWatches.insert( std::make_pair( id, added ) );

	return id;
}

void FileWatcherKqueue::removeWatch( const std::string& directory ) {
	std::string dir( directory );
	FileSystem::dirAddSlashAtEnd( dir );

	Lock lock( mWatchesLock );

	WatchMap::iterator iter = mWatches.begin();

	for ( ; iter != mWatches.end(); ++iter ) {
		if ( dir == iter->second->Directory ) {
			removeWatch( iter->first );
			return;
		}
	}
}

void FileWatcherKqueue::removeWatch( WatchID watchid ) {
	Lock lock( mWatchesLock );

	WatchMap::iterator iter = mWatches.find( watchid );

	if ( iter == mWatches.end() )
		return;

	Watcher* watch = iter->second;

	mWatches.erase( iter );
	mSharedActions.forget( watch );
	mRetiring.release( watch );
}

FileActionBatch* FileWatcherKqueue::sharedActionsFor( bool reportCrossDirectoryMoves ) {
	return reportCrossDirectoryMoves && FileInfo::inodeSupported() ? &mSharedActions : NULL;
}

void FileWatcherKqueue::watch() {
	if ( NULL == mThread ) {
		mThread = new Thread( [this] { run(); } );
		mThread->launch();
	}
}

void FileWatcherKqueue::run() {
	do {
		{
			Lock lock( mWatchesLock );

			// Events of opted-in watches are collected during the scans, and dispatched after.
			std::vector<Watcher*>& scanned = mRetiring.beginPass();
			for ( const auto& entry : mWatches )
				scanned.push_back( entry.second );
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

		System::sleep( mPollingFreq );
	} while ( mInitOK );
}

void FileWatcherKqueue::handleAction( Watcher* watch, const std::string& filename,
									  unsigned long action, const std::string& oldFilename ) {}

std::vector<std::string> FileWatcherKqueue::directories() {
	std::vector<std::string> dirs;

	Lock lock( mWatchesLock );

	dirs.reserve( mWatches.size() );

	WatchMap::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); ++it ) {
		dirs.push_back( it->second->Directory );
	}

	return dirs;
}

bool FileWatcherKqueue::pathInWatches( const std::string& path ) {
	// Watches are added and removed from other threads, and from callbacks.
	Lock lock( mWatchesLock );
	WatchMap::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); ++it ) {
		if ( it->second->Directory == path ) {
			return true;
		}
	}

	return false;
}

void FileWatcherKqueue::addFD() {
	mFileDescriptorCount++;
}

void FileWatcherKqueue::removeFD() {
	mFileDescriptorCount--;
}

bool FileWatcherKqueue::availablesFD() {
	return mFileDescriptorCount <= (Int64)System::getMaxFD() - 500;
}

} // namespace efsw

#endif
