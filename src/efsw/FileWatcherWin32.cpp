#include <efsw/FileWatcherWin32.hpp>
#include <efsw/System.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

namespace efsw
{
	struct WatcherStructWin32
	{
		OVERLAPPED Overlapped;
		WatcherWin32 *	Watch;
	};

#pragma region Internal Functions

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

#pragma endregion

	FileWatcherWin32::FileWatcherWin32() :
		mLastWatchID(0),
		mThread( NULL )
	{
		mInitOK = true;
	}

	FileWatcherWin32::~FileWatcherWin32()
	{
		WatchMap::iterator iter = mWatches.begin();
		WatchMap::iterator end = mWatches.end();
		for(; iter != end; ++iter)
		{
			DestroyWatch(iter->second);
		}
		mWatches.clear();

		mInitOK = false;

		mThread->wait();

		efSAFE_DELETE( mThread );
	}

	WatchID FileWatcherWin32::addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive)
	{
		WatchID watchid = ++mLastWatchID;

		WatcherStructWin32* watch = CreateWatch(directory.c_str(), recursive,
			FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_FILE_NAME);

		if(!watch)
			return Errors::Log::createLastError( Errors::FileNotFound, directory );

		// Add the handle to the handles vector
		mHandles.push_back( watch->Watch->DirHandle );

		watch->Watch->ID = watchid;
		watch->Watch->Watch = this;
		watch->Watch->Listener = watcher;
		watch->Watch->DirName = new char[directory.length()+1];
		strcpy(watch->Watch->DirName, directory.c_str());

		mWatchesLock.lock();
		mWatches.insert(std::make_pair(watchid, watch));
		mWatchesLock.unlock();

		return watchid;
	}

	void FileWatcherWin32::removeWatch(const std::string& directory)
	{
		mWatchesLock.lock();

		WatchMap::iterator iter = mWatches.begin();
		WatchMap::iterator end = mWatches.end();
		for(; iter != end; ++iter)
		{
			if(directory == iter->second->Watch->DirName)
			{
				removeWatch(iter->first);
				return;
			}
		}

		mWatchesLock.unlock();
	}

	void FileWatcherWin32::removeWatch(WatchID watchid)
	{
		mWatchesLock.lock();

		WatchMap::iterator iter = mWatches.find(watchid);

		if(iter == mWatches.end())
			return;

		WatcherStructWin32* watch = iter->second;
		mWatches.erase(iter);

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
		WatchMap::iterator it;

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
					// Don't trust the result - multiple objects may be signalled during a single call.
					// Search for the overlapped folder
					if ( wait_result >=  WAIT_OBJECT_0 && wait_result < WAIT_OBJECT_0 + mHandles.size() )
					{
						mWatchesLock.lock();

						for ( it = mWatches.begin(); it != mWatches.end(); it++ )
						{
							WatcherStructWin32 * watch = (WatcherStructWin32 *)it->second;

							if ( HasOverlappedIoCompleted( &watch->Overlapped ) )
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

						mWatchesLock.unlock();
					}
					else
					{
						//"Unknown return value from WaitForMultipleObjectsEx."
					}
				}
			}
		} while ( mInitOK );
	}

	void FileWatcherWin32::handleAction(Watcher* watch, const std::string& filename, unsigned long action)
	{
		Action fwAction;

		switch(action)
		{
		case FILE_ACTION_RENAMED_NEW_NAME:
		case FILE_ACTION_ADDED:
			fwAction = Actions::Add;
			break;
		case FILE_ACTION_RENAMED_OLD_NAME:
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

		WatchMap::iterator it = mWatches.begin();

		for ( ; it != mWatches.end(); it++ )
		{
			dirs.push_back( std::string( it->second->Watch->DirName ) );
		}

		mWatchesLock.unlock();

		return dirs;
	}
}

#endif
