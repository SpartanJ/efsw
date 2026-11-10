#ifndef EFSW_WATCHERIMPL_HPP
#define EFSW_WATCHERIMPL_HPP

#include <efsw/base.hpp>
#include <efsw/efsw.hpp>
#include <set>
#include <utility>

namespace efsw {

/// Device and inode of a file.
typedef std::pair<Uint64, Uint64> FileIdentity;

/** @brief Base Watcher class */
class Watcher {
  public:
	Watcher();

	Watcher( WatchID id, std::string directory, FileWatchListener* listener, bool recursive );

	virtual ~Watcher() {}

	virtual void watch() {}

	/// @return True when the watch collects its events into the file watcher's shared batch,
	/// to pair moves with other watches. Only polling backends use a shared batch.
	virtual bool collectsSharedActions() const { return false; }

	/// Polling backends: whether the watch's last snapshot lists a file with one of these
	/// identities, meaning that the watch has not yet seen that the file moved away. This is a
	/// lookup in memory, without file system access.
	virtual bool snapshotHasAny( const std::set<FileIdentity>& ) const { return false; }

	WatchID ID;
	std::string Directory;
	FileWatchListener* Listener;
	bool Recursive;
	std::string OldFileName;
};

} // namespace efsw

#endif
