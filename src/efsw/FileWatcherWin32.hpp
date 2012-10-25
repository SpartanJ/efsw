#ifndef _FW_FILEWATCHERWIN32_H_
#define _FW_FILEWATCHERWIN32_H_
#pragma once

#include <efsw/FileWatcherImpl.hpp>

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

#include <map>

namespace efsw
{
	/// Internal watch data
	struct WatcherStructWin32;

	class WatcherWin32 : public Watcher
	{
		public:
			WatcherStructWin32 * Struct;
			HANDLE DirHandle;
			BYTE mBuffer[32 * 1024];
			LPARAM lParam;
			DWORD NotifyFilter;
			bool StopNow;
			FileWatcherImpl* Watch;
			char* DirName;
	};

	/// Implementation for Win32 based on ReadDirectoryChangesW.
	/// @class FileWatcherWin32
	class FileWatcherWin32 : public FileWatcherImpl
	{
	public:
		/// type for a map from WatchID to WatcherWin32 pointer
		typedef std::map<WatchID, WatcherStructWin32*> WatchMap;

	public:
		FileWatcherWin32();

		virtual ~FileWatcherWin32();

		/// Add a directory watch
		/// @exception FileNotFoundException Thrown when the requested directory does not exist
		WatchID addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive);

		/// Remove a directory watch. This is a brute force lazy search O(nlogn).
		void removeWatch(const std::string& directory);

		/// Remove a directory watch. This is a map lookup O(logn).
		void removeWatch(WatchID watchid);

		/// Updates the watcher. Must be called often.
		void watch();

		/// Handles the action
		void handleAction(Watcher* watch, const std::string& filename, unsigned long action);

		/// @return Returns a list of the directories that are being watched
		std::list<std::string> directories();
	private:
		/// Map of WatchID to WatcherWin32 pointers
		WatchMap mWatches;
		/// The last watchid
		WatchID mLastWatchID;

        Thread * mThread;

        Mutex mWatchesLock;
    private:
        void run();
	};
}

#endif

#endif
