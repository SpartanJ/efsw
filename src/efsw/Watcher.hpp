#ifndef EFSW_WATCHERIMPL_HPP
#define EFSW_WATCHERIMPL_HPP

#include <efsw/base.hpp>
#include <efsw/efsw.hpp>

namespace efsw {

/** @brief Base Watcher class */
class Watcher {
  public:
	Watcher();

	Watcher( WatchID id, std::string directory, FileWatchListener* listener, bool recursive );

	virtual ~Watcher() {}

	virtual void watch() {}

	/// Polling backends: the watch was removed while its polling pass is running, possibly
	/// from one of its own callbacks. It must not report anything more; it is deleted once the
	/// pass ends. See RetiringWatches.
	virtual void retire() { Retired = true; }

	WatchID ID;
	std::string Directory;
	FileWatchListener* Listener;
	bool Recursive;
	std::string OldFileName;
	/// Set by retire().
	bool Retired{ false };
};

} // namespace efsw

#endif
