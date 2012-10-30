#ifndef EFSW_FILEWATCHERGENERIC_HPP
#define EFSW_FILEWATCHERGENERIC_HPP

#include <efsw/FileWatcherImpl.hpp>
#include <efsw/WatcherGeneric.hpp>
#include <efsw/DirWatcherGeneric.hpp>
#include <list>

namespace efsw
{

/// Implementation for Generic File Watcher.
/// @class FileWatcherGeneric
class FileWatcherGeneric : public FileWatcherImpl
{
	public:
		typedef std::list<WatcherGeneric*> WatchList;

		FileWatcherGeneric();

		virtual ~FileWatcherGeneric();

		/// Add a directory watch
		/// On error returns WatchID with Error type.
		WatchID addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive);

		/// Remove a directory watch. This is a brute force lazy search O(nlogn).
		void removeWatch(const std::string& directory);

		/// Remove a directory watch. This is a map lookup O(logn).
		void removeWatch(WatchID watchid);

		/// Updates the watcher. Must be called often.
		void watch();

		/// Handles the action
		void handleAction(Watcher * watch, const std::string& filename, unsigned long action);

		/// @return Returns a list of the directories that are being watched
		std::list<std::string> directories();
	protected:
		/// Map of WatchID to WatchStruct pointers
		WatchList mWatches;

		/// The last watchid
		WatchID mLastWatchID;

		Thread * mThread;

		Mutex mWatchesLock;
	private:
		void run();
};

}

#endif
