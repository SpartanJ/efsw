#include <efsw/FileWatcherWin32.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/System.hpp>
#include <efsw/String.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

namespace efsw
{

FileWatcherWin32::FileWatcherWin32( FileWatcher * parent ) :
	FileWatcherImpl( parent ),
	mLastWatchID(0),
	mThread( NULL )
{
	mInitOK = true;
}

FileWatcherWin32::~FileWatcherWin32()
{
	WatchVector::iterator iter = mWatches.begin();

	for(; iter != mWatches.end(); ++iter)
	{
		DestroyWatch((*iter));
	}

	mHandles.clear();
	mWatches.clear();

	mInitOK = false;

	efSAFE_DELETE( mThread );
}

WatchID FileWatcherWin32::addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive)
{
	std::string dir( directory );

	FileInfo fi( dir );

	if ( !fi.isDirectory() )
	{
		return Errors::Log::createLastError( Errors::FileNotFound, dir );
	}
	else if ( !fi.isReadable() )
	{
		return Errors::Log::createLastError( Errors::FileNotReadable, dir );
	}

	FileSystem::dirAddSlashAtEnd( dir );

	WatchID watchid = ++mLastWatchID;

	WatcherStructWin32 * watch = CreateWatch( String::fromUtf8( dir ).toWideString().c_str(), recursive,		FILE_NOTIFY_CHANGE_CREATION |
																			FILE_NOTIFY_CHANGE_LAST_WRITE |
																			FILE_NOTIFY_CHANGE_FILE_NAME |
																			FILE_NOTIFY_CHANGE_DIR_NAME
	);

	if( NULL == watch )
	{
		return Errors::Log::createLastError( Errors::FileNotFound, dir );
	}

	if ( pathInWatches( dir ) )
	{
		return Errors::Log::createLastError( Errors::FileRepeated, dir );
	}

	// Add the handle to the handles vector
	watch->Watch->ID = watchid;
	watch->Watch->Watch = this;
	watch->Watch->Listener = watcher;
	watch->Watch->DirName = new char[dir.length()+1];
	strcpy(watch->Watch->DirName, dir.c_str());

	mWatchesLock.lock();
	mHandles.push_back( watch->Watch->DirHandle );
	mWatches.push_back( watch );
	mWatchesLock.unlock();

	return watchid;
}

void FileWatcherWin32::removeWatch(const std::string& directory)
{
	mWatchesLock.lock();

	WatchVector::iterator iter = mWatches.begin();

	for(; iter != mWatches.end(); ++iter)
	{
		if(directory == (*iter)->Watch->DirName)
		{
			removeWatch((*iter)->Watch->ID);
			return;
		}
	}

	mWatchesLock.unlock();
}

void FileWatcherWin32::removeWatch(WatchID watchid)
{
	mWatchesLock.lock();

	WatchVector::iterator iter = mWatches.begin();

	WatcherStructWin32* watch = NULL;

	for(; iter != mWatches.end(); ++iter)
	{
		// Find the watch ID
		if ( (*iter)->Watch->ID == watchid )
		{
			watch	= (*iter);

			mWatches.erase( iter );

			// Remove handle from the handle vector
			HandleVector::iterator it = mHandles.begin();

			for ( ; it != mHandles.end(); it++ )
			{
				if ( watch->Watch->DirHandle == (*it) )
				{
					mHandles.erase( it );
					break;
				}
			}

			DestroyWatch(watch);

			break;
		}
	}

	mWatchesLock.unlock();
}

void FileWatcherWin32::watch()
{
	if ( NULL == mThread )
	{
		mThread = new Thread( &FileWatcherWin32::run, this );
		mThread->launch();
	}
}

void FileWatcherWin32::run()
{
	if ( mHandles.empty() )
	{
		return;
	}

	do
	{
		if ( !mHandles.empty() )
		{
			DWORD wait_result = WaitForMultipleObjectsEx( mHandles.size(), &mHandles[0], FALSE, 1000, FALSE );

			switch ( wait_result )
			{
				case WAIT_ABANDONED_0:
				case WAIT_ABANDONED_0 + 1:
					//"Wait abandoned."
					break;
				case WAIT_TIMEOUT:
					break;
				case WAIT_FAILED:
					//"Wait failed."
					break;
				default:
				{
					mWatchesLock.lock();

					// Don't trust the result - multiple objects may be signalled during a single call.
					if ( wait_result >=  WAIT_OBJECT_0 && wait_result < WAIT_OBJECT_0 + mWatches.size() )
					{
						WatcherStructWin32 * watch = mWatches[ wait_result ];

						// First ensure that the handle is the same, this means that the watch was not removed.
						if ( mHandles[ wait_result ] == watch->Watch->DirHandle && HasOverlappedIoCompleted( &watch->Overlapped ) )
						{
							DWORD bytes;

							if ( GetOverlappedResult( watch->Watch->DirHandle, &watch->Overlapped, &bytes, FALSE ) )
							{
								WatchCallback( ERROR_SUCCESS, bytes, &watch->Overlapped );
							}
							else
							{
								//"GetOverlappedResult failed."
							}
						}
					}
					else
					{
						//"Unknown return value from WaitForMultipleObjectsEx."
					}

					mWatchesLock.unlock();
				}
			}
		}
		else
		{
			// Wait for a new handle to be added
			System::sleep( 10 );
		}
	} while ( mInitOK );
}

void FileWatcherWin32::handleAction(Watcher* watch, const std::string& filename, unsigned long action, std::string oldFilename)
{
	Action fwAction;

	switch(action)
	{
	case FILE_ACTION_RENAMED_OLD_NAME:
		watch->OldFileName = filename;
		return;
	case FILE_ACTION_ADDED:
		fwAction = Actions::Add;
		break;
	case FILE_ACTION_RENAMED_NEW_NAME:
	{
		fwAction = Actions::Moved;

		std::string fpath( watch->Directory + filename );

		// Update the directory path
		if ( watch->Recursive && FileSystem::isDirectory( fpath ) )
		{
			// Update the new directory path
			std::string opath( watch->Directory + watch->OldFileName );
			FileSystem::dirAddSlashAtEnd( opath );
			FileSystem::dirAddSlashAtEnd( fpath );

			for ( WatchVector::iterator it = mWatches.begin(); it != mWatches.end(); it++ )
			{
				if ( (*it)->Watch->Directory == opath )
				{
					(*it)->Watch->Directory = fpath;

					break;
				}
			}
		}

		watch->Listener->handleFileAction(watch->ID, static_cast<WatcherWin32*>( watch )->DirName, filename, fwAction, watch->OldFileName);
		return;
	}
	case FILE_ACTION_REMOVED:
		fwAction = Actions::Delete;
		break;
	case FILE_ACTION_MODIFIED:
		fwAction = Actions::Modified;
		break;
	};

	watch->Listener->handleFileAction(watch->ID, static_cast<WatcherWin32*>( watch )->DirName, filename, fwAction);
}

std::list<std::string> FileWatcherWin32::directories()
{
	std::list<std::string> dirs;

	mWatchesLock.lock();

	for ( WatchVector::iterator it = mWatches.begin(); it != mWatches.end(); it++ )
	{
		dirs.push_back( std::string( (*it)->Watch->DirName ) );
	}

	mWatchesLock.unlock();

	return dirs;
}

bool FileWatcherWin32::pathInWatches( const std::string& path )
{
	for ( WatchVector::iterator it = mWatches.begin(); it != mWatches.end(); it++ )
	{
		if ( (*it)->Watch->DirName == path )
		{
			return true;
		}
	}

	return false;
}

}

#endif
