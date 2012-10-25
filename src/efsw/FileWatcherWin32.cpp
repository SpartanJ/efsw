#include <efsw/FileWatcherWin32.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

#define _WIN32_WINNT 0x0550
#include <windows.h>

#if defined(_MSC_VER)
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")

// disable secure warnings
#pragma warning (disable: 4996)
#endif

namespace efsw
{
	/// Internal watch data
	class WatchStruct
	{
		public:
		OVERLAPPED mOverlapped;
		HANDLE mDirHandle;
		BYTE mBuffer[32 * 1024];
		LPARAM lParam;
		DWORD mNotifyFilter;
		bool mStopNow;
		FileWatcherImpl* mFileWatcher;
		FileWatchListener* mFileWatchListener;
		char* mDirName;
		WatchID mWatchid;
		bool mIsRecursive;
	};

#pragma region Internal Functions

	// forward decl
	bool RefreshWatch(WatchStruct* pWatch, bool _clear = false);

	/// Unpacks events and passes them to a user defined callback.
	void CALLBACK WatchCallback(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
	{
		TCHAR szFile[MAX_PATH];
		PFILE_NOTIFY_INFORMATION pNotify;
		WatchStruct* pWatch = (WatchStruct*) lpOverlapped;
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

				pWatch->mFileWatcher->handleAction(pWatch, szFile, pNotify->Action);

			} while (pNotify->NextEntryOffset != 0);
		}

		if (!pWatch->mStopNow)
		{
			RefreshWatch(pWatch);
		}
	}

	/// Refreshes the directory monitoring.
	bool RefreshWatch(WatchStruct* pWatch, bool _clear)
	{
		return ReadDirectoryChangesW(
			pWatch->mDirHandle, pWatch->mBuffer, sizeof(pWatch->mBuffer), pWatch->mIsRecursive,
			pWatch->mNotifyFilter, NULL, &pWatch->mOverlapped, _clear ? 0 : WatchCallback) != 0;
	}

	/// Stops monitoring a directory.
	void DestroyWatch(WatchStruct* pWatch)
	{
		if (pWatch)
		{
			pWatch->mStopNow = TRUE;

			CancelIo(pWatch->mDirHandle);

			RefreshWatch(pWatch, true);

			if (!HasOverlappedIoCompleted(&pWatch->mOverlapped))
			{
				sleepEx(5, TRUE);
			}

			CloseHandle(pWatch->mOverlapped.hEvent);
			CloseHandle(pWatch->mDirHandle);
			efSAFE_DELETE( pWatch->mDirName );
			HeapFree(GetProcessHeap(), 0, pWatch);
		}
	}

	/// Starts monitoring a directory.
	WatchStruct* CreateWatch(LPCTSTR szDirectory, bool recursive, DWORD mNotifyFilter)
	{
		WatchStruct* pWatch;
		size_t ptrsize = sizeof(*pWatch);
		pWatch = static_cast<WatchStruct*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ptrsize));

		pWatch->mDirHandle = CreateFile(szDirectory, FILE_LIST_DIRECTORY,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, 
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);

		if (pWatch->mDirHandle != INVALID_HANDLE_VALUE)
		{
			pWatch->mOverlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
			pWatch->mNotifyFilter = mNotifyFilter;
			pWatch->mIsRecursive = recursive;

			if (RefreshWatch(pWatch))
			{
				return pWatch;
			}
			else
			{
				CloseHandle(pWatch->mOverlapped.hEvent);
				CloseHandle(pWatch->mDirHandle);
			}
		}

		HeapFree(GetProcessHeap(), 0, pWatch);
		return NULL;
	}

#pragma endregion

	FileWatcherWin32::FileWatcherWin32()
		: mLastWatchID(0)
	{
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
	}

	WatchID FileWatcherWin32::addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive)
	{
		WatchID watchid = ++mLastWatchID;

		WatchStruct* watch = CreateWatch(directory.c_str(), recursive,
			FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_FILE_NAME);

		if(!watch)
			return Errors::Log::createLastError( Errors::FileNotFound, directory );

		watch->mWatchid = watchid;
		watch->mFileWatcher = this;
		watch->mFileWatchListener = watcher;
		watch->mDirName = new char[directory.length()+1];
		strcpy(watch->mDirName, directory.c_str());

		mWatches.insert(std::make_pair(watchid, watch));

		return watchid;
	}

	void FileWatcherWin32::removeWatch(const std::string& directory)
	{
		WatchMap::iterator iter = mWatches.begin();
		WatchMap::iterator end = mWatches.end();
		for(; iter != end; ++iter)
		{
			if(directory == iter->second->mDirName)
			{
				removeWatch(iter->first);
				return;
			}
		}
	}

	void FileWatcherWin32::removeWatch(WatchID watchid)
	{
		WatchMap::iterator iter = mWatches.find(watchid);

		if(iter == mWatches.end())
			return;

		WatchStruct* watch = iter->second;
		mWatches.erase(iter);

		DestroyWatch(watch);
	}

	void FileWatcherWin32::watch()
	{
		/// @TODO: Implement threaded watch()
		MsgWaitForMultipleObjectsEx(0, NULL, 0, QS_ALLINPUT, MWMO_ALERTABLE);
	}

	void FileWatcherWin32::handleAction(WatchStruct* watch, const std::string& filename, unsigned long action)
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

		watch->mFileWatchListener->handleFileAction(watch->mWatchid, watch->mDirName, filename, fwAction);
	}

	std::list<std::string> FileWatcherWin32::directories()
	{
		std::list<std::string> dirs;

		mWatchesLock.lock();

		WatchMap::iterator it = mWatches.begin();

		for ( ; it != mWatches.end(); it++ )
		{
			dirs.push_back( std::string( it->second->mDirName ) );
		}

		mWatchesLock.unlock();

		return dirs;
	}
}

#endif
