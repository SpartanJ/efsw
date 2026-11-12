#ifndef EFSW_FILEACTIONBATCH_HPP
#define EFSW_FILEACTIONBATCH_HPP

#include <efsw/FileInfo.hpp>
#include <efsw/Watcher.hpp>
#include <efsw/efsw.hpp>
#include <map>
#include <string>
#include <vector>

namespace efsw {

class FileActionBatch {
  public:
	void clear();

	void add( WatchID watchid, const std::string& directory, const std::string& filename,
			  Action action, const std::string& oldFilename, const FileInfo& fileInfo,
			  FileWatchListener* listener = NULL );

	void dispatch( FileWatchListener* listener, bool detectMoves = true );

	/// Polling backends scan their watches one after another, so a rename that happens between
	/// the scans of its source and destination watches leaves each half in a different scan.
	/// A shared batch is filled in passes: beginPass(), then scanned() after each watch's scan,
	/// in scan order, then finishPass(), which completes such split moves before dispatching:
	/// - An Add whose source is still listed in the snapshot of a watch scanned earlier: that
	///   watch is scanned again, so the Delete joins the pass. Other Adds cost nothing.
	/// - A Delete collected after other watches were scanned may have its Add in one of them,
	///   on their next scan. It is held back to the next pass, with the later events of its
	///   watch so that their order is kept.
	/// A move complete in the pass is held or kept as a whole, with its destination: it is
	/// never split across passes, and its Moved event never overtakes earlier events of the
	/// destination watch, such as the Add of its new parent directory.

	/// Starts a polling pass. The events held back by the previous pass come first.
	void beginPass();

	/// Records the end of a watch's scan.
	void scanned( Watcher* watcher );

	/// Completes the moves split between scans of this pass, then dispatches.
	void finishPass();

	/// Cancels every collected event, including those not yet delivered by a dispatch in
	/// progress.
	void cancel();

	/// Drops the events of a watch that is being removed, including those not yet delivered
	/// by a dispatch in progress: a listener may remove another watch, or its own.
	void forget( Watcher* watcher );

  protected:
	struct Event {
		WatchID Watch;
		std::string Directory;
		std::string Filename;
		Action ActionType;
		/// Set when the watch that collected the event is removed. Fills the padding after
		/// ActionType.
		bool Canceled{ false };
		std::string OldFilename;
		FileInfo Info;
		FileWatchListener* Listener;
		/// The scanned watch that collected the event, set by passes of a shared batch.
		/// Identifies the watch even when IDs collide, as kqueue sub-watch IDs do.
		Watcher* Owner{ NULL };

		Event( WatchID watchid, const std::string& directory, const std::string& filename,
			   Action action, const std::string& oldFilename, const FileInfo& fileInfo,
			   FileWatchListener* listener );
	};

	/// The events one watch collected during a pass, as positions in mEvents. Owner is NULL
	/// once the watch is removed.
	struct Segment {
		Watcher* Owner;
		size_t Begin;
		size_t End;
	};

	struct Halves {
		bool Added{ false };
		bool Deleted{ false };

		bool paired() const { return Added && Deleted; }
	};

	typedef std::map<FileIdentity, Halves> HalvesMap;

	/// The moves found in the batch, by position.
	struct Moves {
		static constexpr size_t None = static_cast<size_t>( -1 );
		/// For the halves of a move, and the events inside a moved directory, which the Moved
		/// event reports: the Add position of that move. None for other events.
		std::vector<size_t> MoveOf;
		/// The Delete position of each move, by its Add position.
		std::map<size_t, size_t> SourceOf;
	};

	std::vector<Event> mEvents;
	std::vector<Event> mHeld;
	std::vector<Segment> mSegments;
	/// Events before this position were held by the previous pass, and are not held again.
	size_t mReleased{ 0 };

	void addHalves( HalvesMap& halves, size_t begin ) const;

	/// @return False when the batch has no move.
	bool matchMoves( Moves& moves ) const;

	/// Delivers the events, as Moved events for moves when given. Clears the batch.
	void deliver( FileWatchListener* listener, const Moves* moves );

	/// Sets the owner of the events collected since begin, and records them as a segment.
	void addSegment( Watcher* owner, size_t begin );

	/// Scans again the earlier watches whose snapshot still lists the source of a later Add.
	/// @return For each segment, whether its watch was scanned again.
	std::vector<bool> rescanMoveSources( HalvesMap& halves );

	/// @return True when events were held.
	bool holdMoveSources( const HalvesMap& halves, const std::vector<bool>& rescanned,
						  const Moves& moves );
};

} // namespace efsw

#endif
