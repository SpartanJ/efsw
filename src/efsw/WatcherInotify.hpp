#ifndef EFSW_WATCHERINOTIFY_HPP
#define EFSW_WATCHERINOTIFY_HPP

#include <atomic>
#include <efsw/FileInfo.hpp>
#include <efsw/FileWatcherImpl.hpp>
#include <vector>

namespace efsw {

class WatcherInotify : public Watcher {
  public:
	WatcherInotify();

	/// The watch of the parent directory, in the same registration, or NULL for a root.
	WatcherInotify* Parent;
	/// The watches of the subdirectories, in the same registration. Parent, Children and
	/// IndexInParent are changed under the file watcher's mWatchesLock.
	std::vector<WatcherInotify*> Children;
	/// The position of this watch in Parent->Children, to unlink it in constant time.
	size_t IndexInParent{ 0 };
	WatchID InotifyID;

	FileInfo DirInfo;
	bool syntheticEvents{ false };
	bool reportCrossDirectoryMoves{ false };
	/// Set when the logical watch is unregistered. Its deletion is deferred until the current
	/// event batch is processed, so pointers held by that batch stay valid but must be skipped.
	/// Atomic: the event thread reads it without mWatchesLock, while removeWatch() may set it
	/// from another thread.
	std::atomic<bool> Removed{ false };
};

} // namespace efsw

#endif
