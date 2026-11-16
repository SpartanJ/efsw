#ifndef EFSW_FILEWATCHEROSX_HPP
#define EFSW_FILEWATCHEROSX_HPP

#include <atomic>
#include <efsw/FileWatcherImpl.hpp>
#include <efsw/RetiringWatches.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE || EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <efsw/WatcherKqueue.hpp>

namespace efsw {

/// Implementation for OSX based on kqueue.
/// @class FileWatcherKqueue
class FileWatcherKqueue : public FileWatcherImpl {
	friend class WatcherKqueue;

  public:
	FileWatcherKqueue( FileWatcher* parent, unsigned int pollingFreq );

	virtual ~FileWatcherKqueue();

	/// Add a directory watch
	/// On error returns WatchID with Error type.
	WatchID addWatch( const std::string& directory, FileWatchListener* watcher, bool recursive,
					  const std::vector<WatcherOption>& options ) override;

	/// Remove a directory watch. This is a brute force lazy search O(nlogn).
	void removeWatch( const std::string& directory ) override;

	/// Remove a directory watch. This is a map lookup O(logn).
	void removeWatch( WatchID watchid ) override;

	/// Updates the watcher. Must be called often.
	void watch() override;

	/// Handles the action
	void handleAction( Watcher* watch, const std::string& filename, unsigned long action,
					   const std::string& oldFilename = "" ) override;

	/// @return Returns a list of the directories that are being watched
	std::vector<std::string> directories() override;

  protected:
	/// Map of WatchID to WatchStruct pointers
	WatchMap mWatches;

	/// time out data
	struct timespec mTimeOut;

	/// WatchID allocator
	int mLastWatchID;

	Thread* mThread;

	Mutex mWatchesLock;

	std::vector<WatchID> mRemoveList;

	/// Changed by addWatch(), from any thread, and by the polling thread's rescans.
	std::atomic<long> mFileDescriptorCount;

	unsigned int mPollingFreq{ 500 };
	/// Events of every watch that enabled Options::ReportCrossDirectoryMoves, collected during
	/// one polling pass so moves between those watches can be paired.
	FileActionBatch mSharedActions;
	RetiringWatches mRetiring;

	/// @return The batch a new watch must collect into, or NULL.
	FileActionBatch* sharedActionsFor( bool reportCrossDirectoryMoves );

	bool pathInWatches( const std::string& path ) override;

	void addFD();

	void removeFD();

	bool availablesFD();

  private:
	void run();
};

} // namespace efsw

#endif

#endif
