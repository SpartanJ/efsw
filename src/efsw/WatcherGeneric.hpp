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
	/// The registration that owns the events collected into SharedActionBatch: this watch,
	/// or the kqueue watch it is a fallback descendant of. Set once when the watch is created.
	Watcher* SharedActionOwner{ NULL };

	bool collectsSharedActions() const override { return NULL != SharedActionBatch; }

	bool snapshotHasAny( const std::vector<FileIdentity>& identities ) const override;

	bool collectTree( const std::string& directory,
					  std::vector<const FileInfo*>* entries ) override;

	/// Moves the snapshot lists of the watched tree into lists, before the watch is deleted.
	void takeSnapshots( std::vector<FileInfoList>& lists );

	/// A subdirectory left the watch, which reports a Delete for each entry of its tree: while
	/// a pass collects for move pairing, they are the batch's evidence of the tree.
	void gaveUpTree( const FileInfo& directory );

	void retire() override;

	/// The watched directory was moved: every directory of the tree is now below directory.
	void moveDirectory( const std::string& directory );

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
