#include <efsw/FileWatcherWin32.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/System.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

namespace efsw
{

struct WatcherStructWin32
{
	OVERLAPPED Overlapped;
	WatcherWin32 *	Watch;
};

// forward decl
bool RefreshWatch(WatcherStructWin32* pWatch, bool _clear = false);

/// Unpacks events and passes them to a user defined callback.
void CALLBACK WatchCallback(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
{
	TCHAR szFile[MAX_PATH];
	PFILE_NOTIFY_INFORMATION pNotify;
	WatcherStructWin32 * tWatch = (WatcherStructWin32*) lpOverlapped;
	WatcherWin32 * pWatch = tWatch->Watch;
	size_t offset = 0;

	if(dwNumberOfBytesTransfered == 0)
		return;

	if (dwErrorCode == ERROR_SUCCESS)
	{
		do
		{
			pNotify = (PFILE_NOTIFY_INFORMATION) &pWatch->mBuffer[offset];
			offset += pNotify->NextEntryOffset;

#			if defined(UNICODE)
			{
				lstrcpynW(szFile, pNotify->FileName,
					min(MAX_PATH, pNotify->FileNameLength / sizeof(WCHAR) + 1));
			}
#			else
			{
				int count = WideCharToMultiByte(CP_ACP, 0, pNotify->FileName,
					pNotify->FileNameLength / sizeof(WCHAR),
					szFile, MAX_PATH - 1, NULL, NULL);
				szFile[count] = TEXT('\0');
			}
#			endif

			pWatch->Watch->handleAction(pWatch, szFile, pNotify->Action);

		} while (pNotify->NextEntryOffset != 0);
	}

	if (!pWatch->StopNow)
	{
		RefreshWatch(tWatch);
	}
}

/// Refreshes the directory monitoring.
bool RefreshWatch(WatcherStructWin32* pWatch, bool _clear)
{
	return ReadDirectoryChangesW(
				pWatch->Watch->DirHandle,
				pWatch->Watch->mBuffer,
				sizeof(pWatch->Watch->mBuffer),
				pWatch->Watch->Recursive,
				pWatch->Watch->NotifyFilter,
				NULL,
				&pWatch->Overlapped,
				_clear ? 0 : WatchCallback
	) != 0;
}

/// Stops monitoring a directory.
void DestroyWatch(WatcherStructWin32* pWatch)
{
	if (pWatch)
	{
		WatcherWin32 * tWatch = pWatch->Watch;

		tWatch->StopNow = TRUE;

		CancelIo(tWatch->DirHandle);

		RefreshWatch(pWatch, true);

		if (!HasOverlappedIoCompleted(&pWatch->Overlapped))
		{
			SleepEx(5, TRUE);
		}

		CloseHandle(pWatch->Overlapped.hEvent);
		CloseHandle(pWatch->Watch->DirHandle);
		efSAFE_DELETE( pWatch->Watch->DirName );
		efSAFE_DELETE( pWatch->Watch );
		HeapFree(GetProcessHeap(), 0, pWatch);
	}
}

/// Starts monitoring a directory.
WatcherStructWin32* CreateWatch(LPCTSTR szDirectory, bool recursive, DWORD NotifyFilter)
{
	WatcherStructWin32 * tWatch;
	size_t ptrsize = sizeof(*tWatch);
	tWatch = static_cast<WatcherStructWin32*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ptrsize));

	WatcherWin32 * pWatch = new WatcherWin32();
	tWatch->Watch = pWatch;

	pWatch->DirHandle = CreateFile(
							szDirectory,
							GENERIC_READ,
							FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
							NULL,
							OPEN_EXISTING,
							FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
							NULL
						);

	if (pWatch->DirHandle != INVALID_HANDLE_VALUE)
	{
		tWatch->Overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		pWatch->NotifyFilter = NotifyFilter;
		pWatch->Recursive = recursive;

		if (RefreshWatch(tWatch))
		{
			return tWatch;
		}
		else
		{
			CloseHandle(tWatch->Overlapped.hEvent);
			CloseHandle(pWatch->DirHandle);
		}
	}

	HeapFree(GetProcessHeap(), 0, tWatch);
	return NULL;
}

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

	mThread->wait();

	efSAFE_DELETE( mThread );
}

WatchID FileWatcherWin32::addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive)
{
	std::string dir( directory );

	FileSystem::dirAddSlashAtEnd( dir );

	WatchID watchid = ++mLastWatchID;

	WatcherStructWin32 * watch = CreateWatch( dir.c_str(), recursive,	FILE_NOTIFY_CHANGE_CREATION |
																			FILE_NOTIFY_CHANGE_SIZE |
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
	do
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

						break;
					}
				}
				else
				{
					//"Unknown return value from WaitForMultipleObjectsEx."
				}

				mWatchesLock.unlock();
			}
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
		break;
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
