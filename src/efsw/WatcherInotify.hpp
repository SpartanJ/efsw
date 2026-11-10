#ifndef EFSW_WATCHERINOTIFY_HPP
#define EFSW_WATCHERINOTIFY_HPP

#include <efsw/FileInfo.hpp>
#include <efsw/FileWatcherImpl.hpp>

namespace efsw {

class WatcherInotify : public Watcher {
  public:
	WatcherInotify();

	bool inParentTree( WatcherInotify* parent );

	WatcherInotify* Parent;
	WatchID InotifyID;

	FileInfo DirInfo;
	bool syntheticEvents{ false };
	bool reportCrossDirectoryMoves{ false };
	/// Set when the logical watch is unregistered. Its deletion is deferred until the current
	/// event batch is processed, so pointers held by that batch stay valid but must be skipped.
	bool Removed{ false };
};

} // namespace efsw

#endif
