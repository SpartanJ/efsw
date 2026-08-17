#include <efsw/FileSystem.hpp>
#include <efsw/FileWatcherWin32.hpp>
#include <efsw/Lock.hpp>
#include <efsw/String.hpp>
#include <efsw/System.hpp>

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
	watch->Watch->ReportCrossDirectoryMoves = reportCrossDirectoryMoves && recursive;
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
		StopWatch( ( *iter ) );
		mRetiredWatches.insert( *iter );
	}

	mWatches.clear();
}

void FileWatcherWin32::drainRetiredWatches() {
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
			hasWatches = !mWatches.empty() || !mRetiredWatches.empty();
		}
		if ( mInitOK && hasWatches ) {
			DWORD numOfBytes = 0;
			OVERLAPPED* ov = NULL;
			ULONG_PTR compKey = 0;
			DWORD timeout = INFINITE;
			{
				Lock lock( mWatchesLock );
				for ( auto watch : mWatches ) {
					const DWORD pendingTimeout = PendingMoveWaitTimeout( watch->Watch );
					if ( pendingTimeout < timeout )
						timeout = pendingTimeout;
				}
			}
			BOOL res = GetQueuedCompletionStatus( mIOCP, &numOfBytes, &compKey, &ov, timeout );

			if ( ov != NULL ) {
				Lock lock( mWatchesLock );
				auto retired = mRetiredWatches.find( (WatcherStructWin32*)ov );
				if ( retired != mRetiredWatches.end() ) {
					DestroyWatch( *retired );
					mRetiredWatches.erase( retired );
				} else if ( mWatches.find( (WatcherStructWin32*)ov ) != mWatches.end() ) {
					WatchCallback( numOfBytes, ov );
					FlushPendingMoves( ( (WatcherStructWin32*)ov )->Watch );
				}
			} else if ( !res && GetLastError() == WAIT_TIMEOUT ) {
				Lock lock( mWatchesLock );
				for ( auto watch : mWatches )
					FlushPendingMoves( watch->Watch );
			}
		} else {
			System::sleep( 10 );
		}
	} while ( mInitOK );
}

void FileWatcherWin32::handleAction( Watcher* watch, const std::string& filename,
									 unsigned long action, const std::string& oldFilename ) {
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
