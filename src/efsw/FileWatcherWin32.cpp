#include <efsw/FileSystem.hpp>
#include <efsw/FileWatcherWin32.hpp>
#include <efsw/Lock.hpp>
#include <efsw/String.hpp>
#include <efsw/System.hpp>
#include <iterator>
#include <utility>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

namespace efsw {

FileWatcherWin32::FileWatcherWin32( FileWatcher* parent ) :
	FileWatcherImpl( parent ), mLastWatchID( 0 ), mThread( NULL ) {
	mIOCP = CreateIoCompletionPort( INVALID_HANDLE_VALUE, NULL, 0, 1 );
	if ( mIOCP && mIOCP != INVALID_HANDLE_VALUE )
		mInitOK = true;
}

FileWatcherWin32::~FileWatcherWin32() {
	mInitOK = false;
	removeAllWatches();

	if ( mIOCP && mIOCP != INVALID_HANDLE_VALUE ) {
		PostQueuedCompletionStatus( mIOCP, 0, reinterpret_cast<ULONG_PTR>( this ), NULL );
	}

	efSAFE_DELETE( mThread );
	drainRetiredWatches();

	if ( mIOCP )
		CloseHandle( mIOCP );
}

WatchID FileWatcherWin32::addWatch( const std::string& directory, FileWatchListener* watcher,
									bool recursive, const std::vector<WatcherOption>& options ) {
	std::string dir( directory );

	FileInfo fi( dir );

	if ( !fi.isDirectory() ) {
		return Errors::Log::createLastError( Errors::FileNotFound, dir );
	} else if ( !fi.isReadable() ) {
		return Errors::Log::createLastError( Errors::FileNotReadable, dir );
	}

	FileSystem::dirAddSlashAtEnd( dir );

	Lock lock( mWatchesLock );

	if ( pathInWatches( dir ) ) {
		return Errors::Log::createLastError( Errors::FileRepeated, dir );
	}

	WatchID watchid = ++mLastWatchID;

	DWORD bufferSize =
		static_cast<DWORD>( getOptionValue( options, Option::WinBufferSize, 63 * 1024 ) );
	DWORD notifyFilter = static_cast<DWORD>( getOptionValue(
		options, Option::WinNotifyFilter,
		FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
			FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE ) );
	bool preventDeletion = getOptionValue( options, Option::WinPreventDirectoryDeletion, 0 ) != 0;
	bool reportCrossDirectoryMoves =
		getOptionValue( options, Option::ReportCrossDirectoryMoves, 0 ) != 0;

	WatcherStructWin32* watch = CreateWatch( FileSystem::getWidePath( dir ).c_str(), recursive,
											 bufferSize, notifyFilter, mIOCP, preventDeletion );

	if ( NULL == watch ) {
		return Errors::Log::createLastError( Errors::FileNotFound, dir );
	}

	// Add the handle to the handles vector
	watch->Watch->ID = watchid;
	watch->Watch->Directory = dir;
	watch->Watch->Watch = this;
	watch->Watch->Listener = watcher;
	watch->Watch->ReportCrossDirectoryMoves = reportCrossDirectoryMoves;
	watch->Watch->DirName = new char[dir.length() + 1];
	strcpy( watch->Watch->DirName, dir.c_str() );

	mWatches.insert( watch );

	return watchid;
}

void FileWatcherWin32::removeWatch( const std::string& directory ) {
	std::string dir( directory );
	FileSystem::dirAddSlashAtEnd( dir );

	Lock lock( mWatchesLock );

	Watches::iterator iter = mWatches.begin();

	for ( ; iter != mWatches.end(); ++iter ) {
		if ( dir == ( *iter )->Watch->DirName ) {
			removeWatch( *iter );
			break;
		}
	}
}

void FileWatcherWin32::removeWatch( WatchID watchid ) {
	Lock lock( mWatchesLock );

	Watches::iterator iter = mWatches.begin();

	for ( ; iter != mWatches.end(); ++iter ) {
		// Find the watch ID
		if ( ( *iter )->Watch->ID == watchid ) {
			removeWatch( *iter );
			return;
		}
	}
}

void FileWatcherWin32::removeWatch( WatcherStructWin32* watch ) {
	Lock lock( mWatchesLock );

	discardCrossWatchEvents( watch->Watch );
	StopWatch( watch );
	mWatches.erase( watch );
	mRetiredWatches.insert( watch );
}

void FileWatcherWin32::watch() {
	if ( NULL == mThread ) {
		mThread = new Thread( [this] { run(); } );
		mThread->launch();
	}
}

void FileWatcherWin32::removeAllWatches() {
	Lock lock( mWatchesLock );

	Watches::iterator iter = mWatches.begin();

	for ( ; iter != mWatches.end(); ++iter ) {
		discardCrossWatchEvents( ( *iter )->Watch );
		StopWatch( ( *iter ) );
		mRetiredWatches.insert( *iter );
	}

	mWatches.clear();
}

static void releaseRetiredWatches( FileWatcherWin32::Watches& watches ) {
	for ( auto it = watches.begin(); it != watches.end(); ) {
		if ( !( *it )->Watch->IoPending ) {
			DestroyWatch( *it );
			it = watches.erase( it );
		} else {
			++it;
		}
	}
}

void FileWatcherWin32::drainRetiredWatches() {
	releaseRetiredWatches( mRetiredWatches );
	while ( !mRetiredWatches.empty() ) {
		DWORD numOfBytes = 0;
		OVERLAPPED* ov = NULL;
		ULONG_PTR compKey = 0;
		GetQueuedCompletionStatus( mIOCP, &numOfBytes, &compKey, &ov, INFINITE );

		if ( ov != NULL ) {
			Lock lock( mWatchesLock );
			auto retired = mRetiredWatches.find( (WatcherStructWin32*)ov );
			if ( retired != mRetiredWatches.end() ) {
				DestroyWatch( *retired );
				mRetiredWatches.erase( retired );
			}
		}
	}
}

void FileWatcherWin32::run() {
	do {
		bool hasWatches = false;
		{
			Lock lock( mWatchesLock );
			// Callbacks may remove their own watch; reclaim only after they return.
			releaseRetiredWatches( mRetiredWatches );
			hasWatches = !mWatches.empty() || !mRetiredWatches.empty();
		}
		if ( mInitOK && hasWatches ) {
			DWORD numOfBytes = 0;
			OVERLAPPED* ov = NULL;
			ULONG_PTR compKey = 0;
			DWORD timeout = INFINITE;
			{
				Lock lock( mWatchesLock );
				timeout = crossWatchTimeout();
				for ( auto watch : mWatches ) {
					const DWORD pendingTimeout = PendingMoveWaitTimeout( watch->Watch );
					if ( pendingTimeout < timeout )
						timeout = pendingTimeout;
				}
			}
			BOOL res = GetQueuedCompletionStatus( mIOCP, &numOfBytes, &compKey, &ov, timeout );

			if ( ov != NULL ) {
				Lock lock( mWatchesLock );
				( (WatcherStructWin32*)ov )->Watch->IoPending = false;
				auto retired = mRetiredWatches.find( (WatcherStructWin32*)ov );
				if ( retired != mRetiredWatches.end() ) {
					DestroyWatch( *retired );
					mRetiredWatches.erase( retired );
				} else if ( mWatches.find( (WatcherStructWin32*)ov ) != mWatches.end() ) {
					WatchCallback( numOfBytes, ov );
					FlushPendingMoves( ( (WatcherStructWin32*)ov )->Watch );
					flushCrossWatchEvents();
				}
			} else if ( !res && GetLastError() == WAIT_TIMEOUT ) {
				Lock lock( mWatchesLock );
				// A listener called while flushing may remove watches: iterate a copy. A removed
				// watch stays allocated, retired, until its I/O completes.
				mFlushedWatches.assign( mWatches.begin(), mWatches.end() );
				for ( auto watch : mFlushedWatches )
					FlushPendingMoves( watch->Watch );
				flushCrossWatchEvents();
			}
		} else {
			System::sleep( 10 );
		}
	} while ( mInitOK );
}

static constexpr ULONGLONG crossWatchTimeoutMs = 100;

bool FileWatcherWin32::isCrossWatchCandidate( const WatcherWin32* watch, DWORD action,
											  const LARGE_INTEGER& fileId ) {
	return watch->ReportCrossDirectoryMoves && fileId.QuadPart != 0 &&
		   ( action == FILE_ACTION_REMOVED || action == FILE_ACTION_ADDED );
}

void FileWatcherWin32::eraseCrossWatchEvent( CrossWatchEvents::iterator it ) {
	auto ids = mCrossWatchById.equal_range( it->FileId );
	for ( auto id = ids.first; id != ids.second; ++id ) {
		if ( id->second == it ) {
			mCrossWatchById.erase( id );
			break;
		}
	}
	auto names = mCrossWatchByName.equal_range( it->FileName );
	for ( auto name = names.first; name != names.second; ++name ) {
		if ( name->second == it ) {
			mCrossWatchByName.erase( name );
			break;
		}
	}
	mCrossWatchEvents.erase( it );
}

void FileWatcherWin32::emitCrossWatchEvent( CrossWatchEvents::iterator it ) {
	// Erase before calling out: the listener may re-enter and remove watches.
	WatcherWin32* watch = it->Watch;
	const DWORD action = it->Removed ? FILE_ACTION_REMOVED : FILE_ACTION_ADDED;
	std::string fileName( it->FileName );
	eraseCrossWatchEvent( it );
	handleAction( watch, fileName, action );
}

void FileWatcherWin32::holdOrMatchCrossWatchEvent( WatcherWin32* watch, DWORD action,
												   std::string fileName,
												   const LARGE_INTEGER& fileId ) {
	const bool removed = action == FILE_ACTION_REMOVED;
	const LONGLONG id = fileId.QuadPart;

	// Removed and added halves may arrive from different watch handles in either order.
	// An opposite half held by the same watch wins, since nested registrations can each
	// report a half of one move; otherwise the opposite half must be unique.
	CrossWatchEvents::iterator sameWatch = mCrossWatchEvents.end();
	CrossWatchEvents::iterator otherWatch = mCrossWatchEvents.end();
	size_t sameWatchCount = 0;
	size_t otherWatchCount = 0;
	auto candidates = mCrossWatchById.equal_range( id );
	for ( auto candidate = candidates.first; candidate != candidates.second; ++candidate ) {
		CrossWatchEvents::iterator held = candidate->second;
		if ( held->Removed == removed )
			continue;
		if ( held->Watch == watch ) {
			sameWatch = held;
			sameWatchCount++;
		} else {
			otherWatch = held;
			otherWatchCount++;
		}
	}
	CrossWatchEvents::iterator match = mCrossWatchEvents.end();
	if ( sameWatchCount == 1 )
		match = sameWatch;
	else if ( sameWatchCount == 0 && otherWatchCount == 1 )
		match = otherWatch;

	if ( match != mCrossWatchEvents.end() ) {
		WatcherWin32* source = removed ? watch : match->Watch;
		WatcherWin32* destination = removed ? match->Watch : watch;
		std::string sourceName( removed ? fileName : match->FileName );
		std::string destinationName( removed ? match->FileName : fileName );
		std::string sourcePath( std::string( source->DirName ) + sourceName );
		std::string destinationPath( std::string( destination->DirName ) + destinationName );
		if ( sourcePath != destinationPath ) {
			// File IDs are volume-scoped; the destination must still refer to the same
			// unambiguous file or directory on the source volume.
			FileInfo destinationInfo( destinationPath );
			FileInfo sourceRoot( source->DirName );
			if ( destinationInfo.Inode == static_cast<Uint64>( id ) &&
				 destinationInfo.Device == sourceRoot.Device &&
				 ( destinationInfo.isDirectory() ||
				   ( destinationInfo.isRegularFile() && destinationInfo.LinkCount == 1 ) ) ) {
				eraseCrossWatchEvent( match );
				// Older held halves for either path happened before this move.
				flushCrossWatchEventsFor( source, sourceName );
				flushCrossWatchEventsFor( destination, destinationName );
				if ( source == destination ) {
					// A move within one registration uses the same relative-name handling.
					handleAction( destination, destinationName, FILE_ACTION_RENAMED_NEW_NAME,
								  sourceName );
				} else {
					std::string folder = FileSystem::pathRemoveFileName( destinationPath );
					FileSystem::dirAddSlashAtEnd( folder );
					// Flushing the older halves may have run a listener that removed it.
					if ( destination->StopNow )
						return;
					destination->Listener->handleFileAction(
						destination->ID, folder, FileSystem::fileNameFromPath( destinationPath ),
						Actions::Moved, FileSystem::canonicalSourcePath( sourcePath ) );
				}
				return;
			}
		}
	}

	// An unmatched half becomes a normal Add/Delete when its wait interval expires.
	mCrossWatchEvents.push_back(
		CrossWatchEvent{ watch, std::move( fileName ), id, GetTickCount64(), removed,
						 ++mCrossWatchSequence } );
	CrossWatchEvents::iterator held = std::prev( mCrossWatchEvents.end() );
	mCrossWatchById.emplace( id, held );
	mCrossWatchByName.emplace( held->FileName, held );
}

void FileWatcherWin32::flushCrossWatchEventsFor( WatcherWin32* watch,
												 const std::string& fileName ) {
	// Report one half per iteration, oldest first, and search again afterwards: the listener
	// may re-enter and remove watches, which invalidates any iterator kept across the call.
	while ( !mCrossWatchEvents.empty() ) {
		CrossWatchEvents::iterator oldest = mCrossWatchEvents.end();
		auto consider = [&]( const std::string& name ) {
			auto range = mCrossWatchByName.equal_range( name );
			for ( auto it = range.first; it != range.second; ++it ) {
				if ( it->second->Watch == watch &&
					 ( oldest == mCrossWatchEvents.end() ||
					   it->second->Sequence < oldest->Sequence ) )
					oldest = it->second;
			}
		};
		consider( fileName );
		for ( size_t separator = fileName.find_first_of( "\\/" ); separator != std::string::npos;
			  separator = fileName.find_first_of( "\\/", separator + 1 ) )
			consider( fileName.substr( 0, separator ) );
		if ( oldest == mCrossWatchEvents.end() )
			return;
		emitCrossWatchEvent( oldest );
	}
}

DWORD FileWatcherWin32::crossWatchTimeout() const {
	if ( mCrossWatchEvents.empty() )
		return INFINITE;
	const ULONGLONG elapsed = GetTickCount64() - mCrossWatchEvents.front().CreatedAt;
	return elapsed >= crossWatchTimeoutMs
			   ? 0
			   : static_cast<DWORD>( crossWatchTimeoutMs - elapsed );
}

void FileWatcherWin32::flushCrossWatchEvents() {
	const ULONGLONG now = GetTickCount64();
	while ( !mCrossWatchEvents.empty() &&
			now - mCrossWatchEvents.front().CreatedAt >= crossWatchTimeoutMs )
		emitCrossWatchEvent( mCrossWatchEvents.begin() );
}

void FileWatcherWin32::discardCrossWatchEvents( WatcherWin32* watch ) {
	for ( auto it = mCrossWatchEvents.begin(); it != mCrossWatchEvents.end(); ) {
		auto next = std::next( it );
		if ( it->Watch == watch )
			eraseCrossWatchEvent( it );
		it = next;
	}
}

void FileWatcherWin32::handleAction( Watcher* watch, const std::string& filename,
									 unsigned long action, const std::string& oldFilename ) {
	// A listener removed the watch, possibly its own, from an earlier callback: the stopped watch
	// stays allocated until its I/O completes, but must not report anything more.
	if ( static_cast<WatcherWin32*>( watch )->StopNow )
		return;

	Action fwAction;

	switch ( action ) {
		case FILE_ACTION_RENAMED_OLD_NAME:
			watch->OldFileName = filename;
			return;
		case FILE_ACTION_ADDED:
			fwAction = Actions::Add;
			break;
		case FILE_ACTION_RENAMED_NEW_NAME: {
			std::string source( oldFilename.empty() ? watch->OldFileName : oldFilename );
			watch->OldFileName.clear();

			if ( source.empty() ) {
				handleAction( watch, filename, FILE_ACTION_ADDED );
				return;
			}

			std::size_t sourceSep = source.find_last_of( "/\\" );
			std::size_t destinationSep = filename.find_last_of( "/\\" );
			std::string sourceDirectory =
				sourceSep == std::string::npos ? "" : source.substr( 0, sourceSep + 1 );
			std::string destinationDirectory =
				destinationSep == std::string::npos ? "" : filename.substr( 0, destinationSep + 1 );
			bool crossDirectory = sourceDirectory != destinationDirectory;

			if ( crossDirectory &&
				 !static_cast<WatcherWin32*>( watch )->ReportCrossDirectoryMoves ) {
				handleAction( watch, source, FILE_ACTION_REMOVED );
				handleAction( watch, filename, FILE_ACTION_ADDED );
				return;
			}

			std::string folderPath( static_cast<WatcherWin32*>( watch )->DirName );
			std::string realFilename = filename;
			if ( destinationSep != std::string::npos ) {
				folderPath += destinationDirectory;
				realFilename = filename.substr( destinationSep + 1 );
			}
			FileSystem::dirAddSlashAtEnd( folderPath );

			std::string reportedOldFilename = FileSystem::fileNameFromPath( source );
			if ( crossDirectory )
				reportedOldFilename = FileSystem::canonicalSourcePath(
					std::string( static_cast<WatcherWin32*>( watch )->DirName ) + source );

			watch->Listener->handleFileAction( watch->ID, folderPath, realFilename, Actions::Moved,
											   reportedOldFilename );
			return;
		}
		case FILE_ACTION_REMOVED:
			fwAction = Actions::Delete;
			break;
		case FILE_ACTION_MODIFIED:
			fwAction = Actions::Modified;
			break;
		default:
			return;
	};

	std::string folderPath( static_cast<WatcherWin32*>( watch )->DirName );
	std::string realFilename = filename;
	std::size_t sepPos = filename.find_last_of( "/\\" );

	if ( sepPos != std::string::npos ) {
		folderPath += filename.substr( 0, sepPos + 1 < filename.size() ? sepPos + 1 : sepPos );
		realFilename = filename.substr( sepPos + 1 );
	}

	FileSystem::dirAddSlashAtEnd( folderPath );

	watch->Listener->handleFileAction( watch->ID, folderPath, realFilename, fwAction );
}

std::vector<std::string> FileWatcherWin32::directories() {
	std::vector<std::string> dirs;

	Lock lock( mWatchesLock );

	dirs.reserve( mWatches.size() );

	for ( Watches::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
		dirs.push_back( std::string( ( *it )->Watch->DirName ) );
	}

	return dirs;
}

bool FileWatcherWin32::pathInWatches( const std::string& path ) {
	Lock lock( mWatchesLock );

	for ( Watches::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
		if ( ( *it )->Watch->DirName == path ) {
			return true;
		}
	}

	return false;
}

} // namespace efsw

#endif
