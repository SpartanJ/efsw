#ifndef EFSW_WATCHERWIN32_HPP
#define EFSW_WATCHERWIN32_HPP

#include <efsw/FileWatcherImpl.hpp>
#include <efsw/FileInfo.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

#define _WIN32_WINNT 0x0550
#include <windows.h>

#ifdef EFSW_COMPILER_MSVC
	#pragma comment(lib, "comctl32.lib")
	#pragma comment(lib, "user32.lib")
	#pragma comment(lib, "ole32.lib")

	// disable secure warnings
	#pragma warning (disable: 4996)
#endif

namespace efsw
{

class WatcherWin32;

/// Internal watch data
struct WatcherStructWin32
{
	OVERLAPPED Overlapped;
	WatcherWin32 *	Watch;
};

class cLastModifiedEvent
{
	public:
		cLastModifiedEvent() {}
		FileInfo	file;
		std::string fileName;
};

bool RefreshWatch(WatcherStructWin32* pWatch, bool _clear = false);

void CALLBACK WatchCallback(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped);

void DestroyWatch(WatcherStructWin32* pWatch);

WatcherStructWin32* CreateWatch(LPCTSTR szDirectory, bool recursive, DWORD NotifyFilter);

class WatcherWin32 : public Watcher
{
	public:
		WatcherWin32() :
			Struct( NULL ),
			DirHandle( NULL ),
			lParam( 0 ),
			NotifyFilter( 0 ),
			StopNow( false ),
			Watch( NULL ),
			DirName( NULL )
		{
		}

		WatcherStructWin32 * Struct;
		HANDLE DirHandle;
		BYTE mBuffer[32 * 1024];
		LPARAM lParam;
		DWORD NotifyFilter;
		bool StopNow;
		FileWatcherImpl* Watch;
		char* DirName;
		cLastModifiedEvent LastModifiedEvent;
};

}

#endif

#endif
