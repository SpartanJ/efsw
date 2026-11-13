#ifndef EFSW_WATCHERIMPL_HPP
#define EFSW_WATCHERIMPL_HPP

#include <efsw/FileInfo.hpp>
#include <efsw/efsw.hpp>
#include <set>
#include <vector>

namespace efsw {

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

	/// Polling backends: the entries of the tree below directory, as the watch's snapshots list
	/// them, at every depth. Pointers into the snapshots, valid until the watch scans again or is
	/// deleted. This is a lookup in memory, without file system access.
	/// @param entries Where the entries are appended; NULL to only check.
	/// @return False when the watch does not watch inside directory.
	virtual bool collectTree( const std::string& /*directory*/,
							  std::vector<const FileInfo*>* /*entries*/ ) {
		return false;
	}

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
