#ifndef EFSW_WATCHEROSX_HPP
#define EFSW_WATCHEROSX_HPP

#include <efsw/FileWatcherImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE || EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <efsw/DirectorySnapshot.hpp>
#include <efsw/FileActionBatch.hpp>
#include <sys/event.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>

namespace efsw {

class FileWatcherKqueue;
class WatcherKqueue;

typedef struct kevent KEvent;

/// type for a map from WatchID to WatcherKqueue pointer
typedef std::unordered_map<WatchID, Watcher*> WatchMap;

class WatcherKqueue : public Watcher {
  public:
	WatcherKqueue( WatchID watchid, const std::string& dirname, FileWatchListener* listener,
				   bool recursive, FileWatcherKqueue* watcher, WatcherKqueue* parent = NULL,
				   bool reportCrossDirectoryMoves = false );

	virtual ~WatcherKqueue();

	/// @return True when the file is tracked, false when it could not be opened.
	bool addFile( const std::string& name, bool emitEvents = true );

	/// @return True when the file was tracked.
	bool removeFile( const std::string& name, bool emitEvents = true );

	/// Watches the directory at its path again, after it was replaced there.
	void reopenDirectory();

	// called when the directory is actually changed
	// means a file has been added or removed
	// rescans the watched directory adding/removing files and sending notices
	void rescan();

	void handleAction( const std::string& filename, efsw::Action action,
					   const std::string& oldFilename, const FileInfo& fileInfo );

	void handleFolderAction( std::string filename, efsw::Action action,
							 const std::string& oldFilename, const FileInfo& fileInfo );

	void addAll();

	/// Reports the directories of a created directory's tree, each before the directories
	/// inside it. addAll() adds their watches without reporting them.
	void reportDirectories();

	/// Adds a generic watch for a subdirectory, when file descriptors ran out.
	void addGenericWatch( const std::string& dir, FileWatchListener* watcher, bool recursive );

	void removeAll();

	WatchID watchingDirectory( std::string dir );

	void watch() override;

	WatchID addWatch( const std::string& directory, FileWatchListener* watcher, bool recursive,
					  WatcherKqueue* parent );

	void removeWatch( WatchID watchid );

	bool initOK();

	/// Root watches only. When set, events are collected into this batch, owned by the file
	/// watcher, which dispatches it after polling every watch. Set once at creation.
	FileActionBatch* SharedActionBatch{ NULL };

	bool collectsSharedActions() const override { return NULL != SharedActionBatch; }

	bool snapshotHasAny( const std::vector<FileIdentity>& identities ) const override;

	bool collectTree( const std::string& directory,
					  std::vector<const FileInfo*>* entries ) override;

	void retire() override;

	int lastErrno();

  protected:
	WatchMap mWatches;
	int mLastWatchID;

	// index 0 is always the directory
	std::vector<KEvent> mChangeList;
	size_t mChangeListCount;
	DirectorySnapshot mDirSnap;

	/// The descriptor for the kqueue
	int mKqueue;

	FileWatcherKqueue* mWatcher;

	WatcherKqueue* mParent;

	bool mInitOK;
	int mErrno;
	bool mReportCrossDirectoryMoves;
	bool mCollectActions;
	bool mNeedsRecursiveRescan;
	FileActionBatch mActionBatch;

	bool pathInWatches( const std::string& path );

	bool pathInParent( const std::string& path );

	Watcher* findWatcher( std::string path );

	void moveDirectory( std::string oldPath, std::string newPath, const FileInfo& fileInfo,
						bool emitEvents = true );

	void sendDirChanged();

	/// The directory of a watch below this one left it: while the root collects a pass for
	/// move pairing, the snapshots of the watch's tree become the batch's evidence of it.
	void giveUpTree( Watcher* watch, const FileInfo& directory );

	/// Moves the snapshot lists of a watch's tree, kqueue or generic, into lists.
	static void takeSnapshots( Watcher* watch, std::vector<FileInfoList>& lists );

	WatcherKqueue* root();

	void rescanTree();
};

} // namespace efsw

#endif

#endif
