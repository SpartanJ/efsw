#ifndef _FW_FILEWATCHERIMPL_H_
#define _FW_FILEWATCHERIMPL_H_

#include <efsw/base.hpp>
#include <efsw/Thread.hpp>
#include <efsw/Mutex.hpp>

namespace efsw {

class WatchStruct;

class FileWatcherImpl
{
	public:
		FileWatcherImpl() : mInitOK( false ) {}

		virtual ~FileWatcherImpl() {}

		/// Add a directory watch
		/// On error returns WatchID with Error type.
		virtual WatchID addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive) = 0;

		/// Remove a directory watch. This is a brute force lazy search O(nlogn).
		virtual void removeWatch(const std::string& directory) = 0;

		/// Remove a directory watch. This is a map lookup O(logn).
		virtual void removeWatch(WatchID watchid) = 0;

		/// Updates the watcher. Must be called often.
		virtual void watch() = 0;

		/// Handles the action
		virtual void handleAction(WatchStruct* watch, const std::string& filename, unsigned long action) = 0;

		/// @return Returns a list of the directories that are being watched
		virtual std::list<std::string> directories() = 0;

		/// @return true if the backend init successfully
		virtual bool initOK() { return mInitOK; }
	protected:
		bool	mInitOK;
};

}

#endif
