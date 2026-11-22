#ifndef EFSW_WATCHERGENERIC_HPP
#define EFSW_WATCHERGENERIC_HPP

#include <efsw/FileActionBatch.hpp>
#include <efsw/FileWatcherImpl.hpp>

namespace efsw {

class DirWatcherGeneric;
class FileInfo;

class WatcherGeneric : public Watcher {
  public:
	FileWatcherImpl* WatcherImpl;
	DirWatcherGeneric* DirWatch;
	/// The kqueue registration owning this fallback descendant. Supplies the public watch ID
	/// and preserves its snapshots until the native parent resolves a directory relocation.
	Watcher* SharedActionOwner{ NULL };

	void retire() override;

	/// The watched directory was moved: every directory of the tree is now below directory.
	void moveDirectory( const std::string& directory );

	WatcherGeneric( WatchID id, const std::string& directory, FileWatchListener* fwl,
					FileWatcherImpl* fw, bool recursive );

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
