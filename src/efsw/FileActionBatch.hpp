#ifndef EFSW_FILEACTIONBATCH_HPP
#define EFSW_FILEACTIONBATCH_HPP

#include <efsw/FileInfo.hpp>
#include <efsw/Watcher.hpp>
#include <efsw/efsw.hpp>
#include <string>
#include <utility>
#include <vector>

namespace efsw {

class FileActionBatch {
  public:
	void clear();

	void add( WatchID watchid, const std::string& directory, const std::string& filename,
			  Action action, const std::string& oldFilename, const FileInfo& fileInfo,
			  FileWatchListener* listener = NULL );

	void dispatch( FileWatchListener* listener, bool detectMoves = true );

	/// @return The number of collected events.
	size_t size() const { return mEvents.size(); }

	/// Finds the most recently collected Add or Delete that has a filesystem identity but no
	/// opposite half in the batch, so it cannot be paired into a Moved event.
	/// @return True and the event's position in collection order, or false when there is none.
	bool lastUnpairedHalf( size_t& position ) const;

	/// Polling backends scan their watches one after another, so a rename that happens between
	/// the scans of its source and destination watches leaves each half in a different pass.
	/// When the batch has an unpaired half, scans once more the watches that collect into it
	/// and were scanned before the watch that collected that half, so the missing half joins
	/// this batch. scanned holds each watch with the batch size after its scan, in scan order.
	void rescanForUnpairedHalf( const std::vector<std::pair<Watcher*, size_t>>& scanned );

  protected:
	struct Event {
		WatchID Watch;
		std::string Directory;
		std::string Filename;
		Action ActionType;
		std::string OldFilename;
		FileInfo Info;
		FileWatchListener* Listener;

		Event( WatchID watchid, const std::string& directory, const std::string& filename,
			   Action action, const std::string& oldFilename, const FileInfo& fileInfo,
			   FileWatchListener* listener );
	};

	std::vector<Event> mEvents;
};

} // namespace efsw

#endif
