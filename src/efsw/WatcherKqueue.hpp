#ifndef EFSW_WATCHEROSX_HPP
#define EFSW_WATCHEROSX_HPP

#include <efsw/FileWatcherImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE || EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <efsw/DirectorySnapshot.hpp>
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
				   bool recursive, FileWatcherKqueue* watcher, WatcherKqueue* parent = NULL );

	virtual ~WatcherKqueue();

	/// @return True when the file is tracked, false when it could not be opened.
	bool addFile( const std::string& name, bool emitEvents = true );

	/// @return True when the file was tracked.
	bool removeFile( const std::string& name, bool emitEvents = true );

	/// Watches the directory at its path again, after it was replaced there.
	bool reopenDirectory();

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
	/// Identity of the directory descriptor, not of the latest path lookup. A failed reopen
	/// leaves it unchanged, so a later poll retries even if the snapshot already advanced.
	FileIdentity mDirectoryIdentity{ 0, 0 };

	bool mInitOK;
	int mErrno;
	bool mFilesNeedReopen{ false };

	bool pathInWatches( const std::string& path );

	bool pathInParent( const std::string& path );

	Watcher* findWatcher( std::string path );

	void moveDirectory( std::string oldPath, std::string newPath, const FileInfo& fileInfo,
						bool emitEvents = true );

	void sendDirChanged();

	WatcherKqueue* root();

	KEvent* findFile( const std::string& name );

	/// Registers a replacement before closing the old descriptor. Failure leaves it intact.
	bool reopen( KEvent& change, const FileInfo& info );

	bool refreshFile( const FileInfo& info );
};

} // namespace efsw

#endif

#endif
