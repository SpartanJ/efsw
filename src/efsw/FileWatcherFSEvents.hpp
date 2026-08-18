#ifndef EFSW_FILEWATCHERFSEVENTS_HPP
#define EFSW_FILEWATCHERFSEVENTS_HPP

#include <efsw/FileWatcherImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <efsw/WatcherFSEvents.hpp>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace efsw {

/// macOS implementation based on FSEvents.
/// @class FileWatcherFSEvents
class FileWatcherFSEvents : public FileWatcherImpl {
	friend class WatcherFSEvents;

  public:
	/// @return If FSEvents supports file-level notifications ( true if OS X >= 10.7 )
	static bool isGranular();

	/// type for a map from WatchID to logical FSEvents watches
	typedef std::unordered_map<WatchID, std::shared_ptr<WatcherFSEvents>> WatchMap;

	FileWatcherFSEvents( FileWatcher* parent );

	virtual ~FileWatcherFSEvents();

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
	static void FSEventCallback( ConstFSEventStreamRef streamRef, void* userData, size_t numEvents,
								 void* eventPaths, const FSEventStreamEventFlags eventFlags[],
								 const FSEventStreamEventId eventIds[] );

	/// Replaces the current stream with one containing every path in mWatches. mWatchesMutex must
	/// be held by the caller. replacedStream is invalidated and must be released after all
	/// callbacks already queued for it have completed.
	bool rebuildStream( FSEventStreamRef& replacedStream );

	/// Stops and invalidates the active stream. mWatchesMutex must be held by the caller.
	FSEventStreamRef stopStream();

	/// Releases an invalidated stream after callbacks already submitted to the dispatch queue.
	void releaseStreamAfterCallbacks( FSEventStreamRef stream );

	bool pathInWatchesUnlocked( const std::string& path ) const;

	/// Logical watches routed through the shared FSEvent stream.
	WatchMap mWatches;

	/// The last watchid
	WatchID mLastWatchID;

	std::mutex mWatchesMutex;

	FSEventStreamRef mStream;
	dispatch_queue_t mDispatchQueue;
	std::vector<const void*> mStreamPaths;
	std::vector<FSEvent> mEventBuffer;
	bool mWatching;

	bool pathInWatches( const std::string& path ) override;
};

} // namespace efsw

#endif

#endif
