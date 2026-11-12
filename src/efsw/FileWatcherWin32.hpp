#ifndef EFSW_FILEWATCHERWIN32_HPP
#define EFSW_FILEWATCHERWIN32_HPP

#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

#include <efsw/WatcherWin32.hpp>
#include <list>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace efsw {

/// Implementation for Win32 based on ReadDirectoryChangesW.
/// @class FileWatcherWin32
class FileWatcherWin32 : public FileWatcherImpl {
  public:
	/// type for a map from WatchID to WatcherWin32 pointer
	typedef std::unordered_set<WatcherStructWin32*> Watches;

	FileWatcherWin32( FileWatcher* parent );

	virtual ~FileWatcherWin32();

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

	// Cross-watch move correlation. All of these run with mWatchesLock held.

	/// @return True when the event is an Add/Remove half that may belong to a move between
	/// directories and must go through holdOrMatchCrossWatchEvent().
	static bool isCrossWatchCandidate( const WatcherWin32* watch, DWORD action,
									   const LARGE_INTEGER& fileId );
	/// Pairs the half with a held opposite half and reports a Moved event, or holds it until
	/// the opposite half arrives or the wait interval expires.
	void holdOrMatchCrossWatchEvent( WatcherWin32* watch, DWORD action, std::string fileName,
									 const LARGE_INTEGER& fileId );
	/// Reports the held halves of a watch that concern a path or one of its parent
	/// directories, so an event reported immediately cannot overtake them.
	void flushCrossWatchEventsFor( WatcherWin32* watch, const std::string& fileName );
	/// @return Milliseconds until the oldest held half expires, or INFINITE.
	DWORD crossWatchTimeout() const;
	/// Reports every held half whose wait interval expired, in arrival order.
	void flushCrossWatchEvents();
	void discardCrossWatchEvents( WatcherWin32* watch );

  protected:
	HANDLE mIOCP;
	Watches mWatches;
	Watches mRetiredWatches;

	/// The last watchid
	WatchID mLastWatchID;
	Thread* mThread;
	Mutex mWatchesLock;
	struct CrossWatchEvent {
		WatcherWin32* Watch;
		std::string FileName;
		LONGLONG FileId;
		ULONGLONG CreatedAt;
		bool Removed;
		ULONGLONG Sequence;
	};
	typedef std::list<CrossWatchEvent> CrossWatchEvents;
	/// Held Add/Remove halves in arrival order. Arrival order is also expiry order.
	CrossWatchEvents mCrossWatchEvents;
	/// Reused copy of mWatches, flushed when the completion wait times out.
	std::vector<WatcherStructWin32*> mFlushedWatches;
	/// Indexes into mCrossWatchEvents, by file identity and by watch-relative name.
	std::unordered_multimap<LONGLONG, CrossWatchEvents::iterator> mCrossWatchById;
	std::unordered_multimap<std::string, CrossWatchEvents::iterator> mCrossWatchByName;
	ULONGLONG mCrossWatchSequence{ 0 };

	void emitCrossWatchEvent( CrossWatchEvents::iterator it );
	void eraseCrossWatchEvent( CrossWatchEvents::iterator it );

	bool pathInWatches( const std::string& path ) override;

	/// Remove all directory watches.
	void removeAllWatches();
	void drainRetiredWatches();

	void removeWatch( WatcherStructWin32* watch );

  private:
	void run();
};

} // namespace efsw

#endif

#endif
