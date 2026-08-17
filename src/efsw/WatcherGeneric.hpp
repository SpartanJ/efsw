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
