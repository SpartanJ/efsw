#ifndef EFSW_FILEWATCHERLINUX_HPP
#define EFSW_FILEWATCHERLINUX_HPP

#include <efsw/FileWatcherImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY

#include <efsw/WatcherInotify.hpp>
#include <unordered_map>
#include <vector>

namespace efsw {

/// Implementation for Linux based on inotify.
/// @class FileWatcherInotify
class FileWatcherInotify : public FileWatcherImpl {
  public:
	/// type for a map from WatchID to WatchStruct pointer
	typedef std::unordered_map<WatchID, WatcherInotify*> WatchMap;
	typedef std::unordered_map<int, std::vector<WatcherInotify*>> NativeWatchMap;

	FileWatcherInotify( FileWatcher* parent );

	virtual ~FileWatcherInotify();

	/// Add a directory watch
	/// On error returns WatchID with Error type.
	WatchID addWatch( const std::string& directory, FileWatchListener* watcher, bool recursive,
					  const std::vector<WatcherOption>& options ) override;

	/// Remove a directory watch. This is a brute force lazy search O(nlogn).
	void removeWatch( const std::string& directory ) override;

	/// Remove a directory watch. This is a map lookup O(logn).
	void removeWatch( WatchID watchid ) override;

	/// Updates the watcher. Must be called often.
	void watch() override;

	/// Handles the action
	void handleAction( Watcher* watch, const std::string& filename, unsigned long action,
					   const std::string& oldFilename = "" ) override;

	/// @return Returns a list of the directories that are being watched
	std::vector<std::string> directories() override;

  protected:
	/// Logical watches by native inotify descriptor. A native descriptor watches one physical
	/// directory, and normally has one owner. It has several owners when registrations overlap
	/// after a topology change, for example when a directory holding an explicit registration
	/// is moved into a recursive one: inotify returns the existing descriptor for an inode that
	/// is already watched, so both registrations must share it. Events of a descriptor are
	/// delivered to each owner, and the descriptor is removed with its last owner.
	NativeWatchMap mWatches;

	/// User added watches, by WatchID
	WatchMap mRealWatches;

	/// Logical watches by directory, one entry per owner
	std::unordered_multimap<std::string, WatcherInotify*> mWatchesRef;

	/// inotify file descriptor
	int mFD;

	Thread* mThread;

	Mutex mWatchesLock;
	Mutex mRealWatchesLock;
	Mutex mInitLock;
	bool mIsTakingAction;
	std::vector<WatcherInotify*> mDeletedWatches;

	WatchID addWatch( const std::string& directory, FileWatchListener* watcher, bool recursive,
					  bool syntheticEvents, bool reportCrossDirectoryMoves,
					  WatcherInotify* parent = NULL, bool fromInternalEvent = false );

	bool pathInWatches( const std::string& path ) override;

  private:
	/// @return Returns true if child is a strict descendant directory of parent.
	/// Equality is not considered "inside" so callers can distinguish exact
	/// duplicates from strict descendants.
	static bool isDirectoryInside( const std::string& child, const std::string& parent );

	/// Returns a canonical, slash-terminated path when the directory can be
	/// resolved, and a slash-terminated lexical path otherwise.
	static std::string directoryComparisonPath( const std::string& directory );

	/// Returns FileRepeated for an exact duplicate, FileOverlapping for an
	/// unsupported recursive overlap, or NoError when registration is safe.
	/// Read-only, does not modify any state.
	Errors::Error getWatchConflict( const std::string& directory, bool recursive );

	void run();

	void emitCrossDirectoryMove( Watcher* src, const std::string& srcFile, Watcher* dst,
								 const std::string& dstFile );

	/// Reports a directory moved between two registered roots as one Moved event. The source
	/// registration's watches of the moved subtree are handed over to a recursive destination,
	/// or removed for a non-recursive one. Other registrations inside the subtree keep their
	/// native watches, which a recursive destination then shares.
	void moveDirectoryBetweenRoots( WatcherInotify* src, const std::string& srcFile,
									WatcherInotify* dst, const std::string& dstFile );

	/// Adds a logical watch to mWatches and mWatchesRef. mWatchesLock must be held.
	void registerWatchLocked( WatcherInotify* watch );

	/// Removes the mWatchesRef entry of a logical watch. mWatchesLock must be held.
	void unindexWatchLocked( WatcherInotify* watch );

	/// Unregisters a logical watch, and every watch of its tree when it is a recursive root.
	/// The native descriptor is removed with its last owner. mWatchesLock must be held.
	void removeWatchLocked( WatcherInotify* watch, bool skipInotifyRemove = false );

	/// Unregisters one logical watch, without its descendants. The native descriptor is removed
	/// with its last owner, and the watch is deleted after the current event batch.
	/// mWatchesLock must be held.
	void unregisterWatchLocked( WatcherInotify* watch, bool skipInotifyRemove );

	/// @return The watch of the registration for the directory, or NULL. mWatchesLock must be
	/// held.
	WatcherInotify* findWatchLocked( const std::string& directory, WatchID registration );

	/// Copies the current owners of a native descriptor. mWatchesLock must be held.
	void ownersLocked( int wd, std::vector<WatcherInotify*>& owners ) const;

	/// A watched directory moved: every logical watch at or below oldPath, whatever its
	/// registration, now lives below newPath. mWatchesLock must be held.
	void renameWatchedDirectoriesLocked( const std::string& oldPath, const std::string& newPath );

	void checkForNewWatcher( Watcher* watch, std::string fpath );

	/// @return The watch of the registration for the parent directory of dir, or NULL.
	Watcher* watcherContainsDirectory( std::string dir, WatchID registration );
};

} // namespace efsw

#endif

#endif
