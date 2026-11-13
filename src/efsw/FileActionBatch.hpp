#ifndef EFSW_FILEACTIONBATCH_HPP
#define EFSW_FILEACTIONBATCH_HPP

#include <efsw/FileInfo.hpp>
#include <efsw/Watcher.hpp>
#include <efsw/efsw.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace efsw {

class FileActionBatch {
  public:
	void clear();

	/// @param owner The registration that collects the event into a shared batch, the watch
	/// its file watcher scans and removes: forget() cancels its events from then on, even
	/// before its scan ends.
	void add( WatchID watchid, const std::string& directory, const std::string& filename,
			  Action action, const std::string& oldFilename, const FileInfo& fileInfo,
			  FileWatchListener* listener = NULL, Watcher* owner = NULL );

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

	/// While a pass collects: a directory left a watch that watched the tree below it. When the
	/// directory's move to a directory watched below is matched, the two trees are compared,
	/// and the entries renamed, relocated, created or deleted inside it since the last scan are
	/// reported after its Moved event. The entries it kept are reported by that event. Otherwise
	/// the evidence is dropped at the end of the pass: it is never reported on its own.
	/// @param lists The tree's snapshot lists, moved in: no entry is copied. The Deletes the watch
	/// reported for entries of the tree are evidence too: empty when it reported one for each.
	void addSourceTree( const FileInfo& directory, std::vector<FileInfoList> lists );

  protected:
	struct Event {
		WatchID Watch;
		std::string Directory;
		std::string Filename;
		Action ActionType;
		/// Set when the watch that collected the event is removed. Fills the padding after
		/// ActionType.
		bool Canceled{ false };
		/// Set for an event held to the next pass whose entry the changes of a moved tree
		/// reported meanwhile: it is never delivered, and its move is delivered as the half that
		/// was not reported.
		bool Superseded{ false };
		/// Set for the Add of a directory that left a compared tree without a Delete to pair
		/// it, as kqueue reports one: delivered with what it holds.
		bool WithContents{ false };
		std::string OldFilename;
		FileInfo Info;
		FileWatchListener* Listener;
		/// The registration that collected the event into a shared batch. Identifies it even
		/// when IDs collide, as kqueue sub-watch IDs do.
		Watcher* Owner;

		Event( WatchID watchid, const std::string& directory, const std::string& filename,
			   Action action, const std::string& oldFilename, const FileInfo& fileInfo,
			   FileWatchListener* listener, Watcher* owner );
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

	typedef std::unordered_map<FileIdentity, Halves, FileIdentityHash> HalvesMap;

	/// A change found by comparing the trees of a directory move, delivered after its Moved
	/// event. Points into the snapshots or the source evidence, which outlive the delivery: a
	/// watch removed during a pass is deleted after it.
	struct TreeChange {
		Action ActionType;
		/// The entry: at the destination, or for a Delete, at the source.
		const FileInfo* Entry;
		/// For a move, the entry at the source.
		const FileInfo* Source;
		/// The entry's absolute path when reported, unless its own path gives it: for a Delete,
		/// where the directory's Moved event put its source path.
		std::string Path;
		/// For a move, the absolute path it leaves, unless the source entry's path gives it.
		std::string OldPath;
		/// For a Delete of an entry that left the tree for another directory, the Add position
		/// of that move of its own: the Delete is only delivered when that move was not, which
		/// is then delivered as an Add. None for other changes.
		size_t Leaving;
		/// The position in the held events of the event whose entry this change reports, which
		/// it supersedes once delivered. None for other changes.
		size_t Held;
	};

	/// The changes inside the trees of a directory move.
	struct TreeChanges {
		/// The directory's path at the destination, without a final separator.
		std::string DestinationRoot;
		/// The length of the directory's path at the source, without a final separator.
		size_t SourceLength;
		std::vector<TreeChange> Changes;
	};

	/// The moves found in the batch, by position.
	struct Moves {
		static constexpr size_t None = static_cast<size_t>( -1 );
		/// For the halves of a move, and the events inside a moved directory, which the Moved
		/// event reports: the Add position of that move. None for other events.
		std::vector<size_t> MoveOf;
		/// For the Add of a move: the position of its Delete. None for other events.
		std::vector<size_t> SourceOf;
		/// The Add positions of the directory moves whose trees are compared.
		std::vector<size_t> Reconciled;
		/// What changed inside the trees of directory moves, by the move's Add position.
		std::unordered_map<size_t, TreeChanges> Followers;
		/// The directory move whose Moved event a move inside it follows, both by Add position:
		/// its old path is where that event put it.
		std::unordered_map<size_t, size_t> Within;
	};

	std::vector<Event> mEvents;
	std::vector<Event> mHeld;
	/// The snapshot lists of the directory trees that left a watch, by the directory's
	/// identity. See addSourceTree().
	std::unordered_map<FileIdentity, std::vector<FileInfoList>, FileIdentityHash> mSourceTrees;
	std::vector<Segment> mSegments;
	/// Events before this position were held by the previous pass, and are not held again.
	size_t mReleased{ 0 };

	void addHalves( HalvesMap& halves, size_t begin ) const;

	/// @return False when the batch has no move.
	bool matchMoves( Moves& moves ) const;

	/// Places the events that refer to the tree under a moved directory's destination, collected
	/// before its move, right after that move: a backend that scans deeper directories first
	/// collects them first, but they must not reach a listener before the directory exists.
	void followEnclosingMoves( Moves& moves );

	class TreePlanner;

	/// Compares the trees of the directory moves that have evidence on both ends, and sets the
	/// events found as the followers of their Moved events.
	void reconcileTrees( Moves& moves );

	/// Keeps only the evidence of the directories whose Delete was held to the next pass.
	void pruneSourceTrees();

	/// @return The source paths, without a final separator, of the directories whose trees left
	/// evidence, with their identities: a directory inside one has evidence of its own tree, the
	/// part below it.
	std::unordered_map<std::string, FileIdentity> evidenceRoots() const;

	/// Delivers the events, as Moved events for moves when given. Clears the batch.
	void deliver( FileWatchListener* listener, const Moves* moves );

	/// Delivers one event as collected, unless it was canceled.
	void deliverEvent( FileWatchListener* listener, size_t index );

	/// Sets the owner of the events collected since begin that have none, and records them
	/// as a segment.
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
