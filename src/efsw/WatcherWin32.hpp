#ifndef EFSW_WATCHERWIN32_HPP
#define EFSW_WATCHERWIN32_HPP

#include <efsw/FileInfo.hpp>
#include <efsw/FileWatcherImpl.hpp>
#include <vector>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

#include <windows.h>

#ifdef EFSW_COMPILER_MSVC
#pragma comment( lib, "comctl32.lib" )
#pragma comment( lib, "user32.lib" )
#pragma comment( lib, "ole32.lib" )

// disable secure warnings
#pragma warning( disable : 4996 )
#endif

namespace efsw {

class WatcherWin32;

enum RefreshResult { Failed, Success, SucessEx };

/// Internal watch data
struct WatcherStructWin32 {
	OVERLAPPED Overlapped;
	WatcherWin32* Watch;
};

struct sLastModifiedEvent {
	FileInfo file;
	std::string fileName;
};

struct PendingRenameWin32 {
	std::string FileName;
	LARGE_INTEGER FileId;
	ULONGLONG CreatedAt;
};

struct ExtendedEventWin32 {
	DWORD Action;
	std::string FileName;
	LARGE_INTEGER FileId;
};

RefreshResult RefreshWatch( WatcherStructWin32* pWatch );

void CALLBACK WatchCallback( DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped );

DWORD PendingMoveWaitTimeout( const WatcherWin32* pWatch );

void FlushPendingMoves( WatcherWin32* pWatch );

void StopWatch( WatcherStructWin32* pWatch );

void DestroyWatch( WatcherStructWin32* pWatch );

WatcherStructWin32* CreateWatch( LPCWSTR szDirectory, bool recursive, DWORD bufferSize,
								 DWORD notifyFilter, HANDLE iocp, bool preventDeletion );

class WatcherWin32 : public Watcher {
  public:
	WatcherWin32( DWORD dwBufferSize ) :
		Struct( NULL ),
		DirHandle( NULL ),
		Buffer(),
		ExtendedEvents(),
		lParam( 0 ),
		NotifyFilter( 0 ),
		StopNow( false ),
		Extended( false ),
		ReportCrossDirectoryMoves( false ),
		Watch( NULL ),
		DirName( NULL ) {
		Buffer.resize( dwBufferSize );
	}

	WatcherStructWin32* Struct;
	HANDLE DirHandle;
	std::vector<BYTE> Buffer;
	std::vector<ExtendedEventWin32> ExtendedEvents;
	LPARAM lParam;
	DWORD NotifyFilter;
	bool StopNow;
	bool Extended;
	bool ReportCrossDirectoryMoves;
	FileWatcherImpl* Watch;
	char* DirName;
	sLastModifiedEvent LastModifiedEvent;
	std::vector<PendingRenameWin32> PendingRenames;
	std::vector<PendingRenameWin32> PendingRemovals;
};

} // namespace efsw

#endif

#endif
