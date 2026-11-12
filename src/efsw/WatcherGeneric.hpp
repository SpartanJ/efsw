#ifndef EFSW_WATCHERGENERIC_HPP
#define EFSW_WATCHERGENERIC_HPP

#include <efsw/FileActionBatch.hpp>
#include <efsw/FileWatcherImpl.hpp>

namespace efsw {

class DirWatcherGeneric;

class WatcherGeneric : public Watcher {
  public:
	FileWatcherImpl* WatcherImpl;
	DirWatcherGeneric* DirWatch;
	bool ReportCrossDirectoryMoves;
	/// When set, scans collect events into this batch, owned by the file watcher, which
	/// dispatches it after scanning every watch. Set once when the watch is created.
	FileActionBatch* SharedActionBatch{ NULL };

	bool collectsSharedActions() const override { return NULL != SharedActionBatch; }

	bool snapshotHasAny( const std::set<FileIdentity>& identities ) const override;

	void retire() override;

	WatcherGeneric( WatchID id, const std::string& directory, FileWatchListener* fwl,
					FileWatcherImpl* fw, bool recursive, bool reportCrossDirectoryMoves = false );

	~WatcherGeneric();

	void watch() override;

	void watchDir( std::string dir );

	bool pathInWatches( std::string path );

	void handleAction( const std::string& directory, const std::string& filename, Action action,
					   const std::string& oldFilename, const FileInfo& fileInfo );

  protected:
	FileActionBatch mActionBatch;
	bool mCollectActions{ false };
};

} // namespace efsw

#endif
