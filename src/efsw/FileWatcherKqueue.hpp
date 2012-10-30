#ifndef _FW_FILEWATCHEROSX_H_
#define _FW_FILEWATCHEROSX_H_
#pragma once

#include <efsw/FileWatcherImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE

#include <efsw/WatcherKqueue.hpp>

namespace efsw
{
	/// Implementation for OSX based on kqueue.
	/// @class FileWatcherKqueue
	class FileWatcherKqueue : public FileWatcherImpl
	{
		friend class WatcherKqueue;
	public:
		FileWatcherKqueue();

		virtual ~FileWatcherKqueue();

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
		/// Map of WatchID to WatchStruct pointers
		WatchMap mWatches;

		/// time out data
		struct timespec mTimeOut;

		/// WatchID allocator
		int mLastWatchID;

		Thread * mThread;

		Mutex mWatchesLock;

		std::list<WatchID> mRemoveList;

		void run();
	};
}

#endif

#endif
