#ifndef EFSW_FILEACTIONBATCH_HPP
#define EFSW_FILEACTIONBATCH_HPP

#include <efsw/FileInfo.hpp>
#include <efsw/Watcher.hpp>
#include <efsw/efsw.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

	/// The Adds and Deletes of an identity, candidate halves of a move: how many of each, and
	/// the position of the last one.
	struct Candidates {
		size_t Added{ 0 };
		size_t Deleted{ 0 };
		size_t Created{ 0 };
		size_t Removed{ 0 };
	};

	/// Values by identity, in a hash table kept from pass to pass: clearing it keeps its memory,
	/// so that inserting allocates nothing until it holds more identities than before. Visited
	/// in insertion order.
	template <typename Value> class IdentityTable {
	  public:
		void clear() {
			for ( size_t slot : mUsed )
				mSlots[slot].Used = false;
			mUsed.clear();
		}

		bool empty() const { return mUsed.empty(); }

		Value& operator[]( const FileIdentity& identity ) {
			if ( 2 * ( mUsed.size() + 1 ) > mSlots.size() )
				grow();
			size_t slot = slotOf( identity );
			while ( mSlots[slot].Used ) {
				if ( mSlots[slot].Key == identity )
					return mSlots[slot].Item;
				slot = ( slot + 1 ) & ( mSlots.size() - 1 );
			}
			mSlots[slot].Used = true;
			mSlots[slot].Key = identity;
			mSlots[slot].Item = Value();
			mUsed.push_back( slot );
			return mSlots[slot].Item;
		}

		/// @return The identity's value, or NULL.
		const Value* find( const FileIdentity& identity ) const {
			if ( mSlots.empty() )
				return NULL;
			for ( size_t slot = slotOf( identity ); mSlots[slot].Used;
				  slot = ( slot + 1 ) & ( mSlots.size() - 1 ) )
				if ( mSlots[slot].Key == identity )
					return &mSlots[slot].Item;
			return NULL;
		}

		/// Calls visit with each value, in insertion order.
		template <typename Visit> void forEach( Visit visit ) const {
			for ( size_t slot : mUsed )
				visit( mSlots[slot].Item );
		}

	  private:
		struct Slot {
			FileIdentity Key;
			Value Item;
			bool Used{ false };
		};

		/// A power of two of slots, at most half used.
		std::vector<Slot> mSlots;
		/// The slots in use, in insertion order.
		std::vector<size_t> mUsed;

		/// Fibonacci hashing: spreads identities whose hashes share their low bits.
		size_t slotOf( const FileIdentity& identity ) const {
			const unsigned long long mixed =
				static_cast<unsigned long long>( FileIdentityHash()( identity ) ) *
				0x9E3779B97F4A7C15ull;
			return static_cast<size_t>( mixed >> 32 ) & ( mSlots.size() - 1 );
		}

		void grow() {
			std::vector<Slot> old;
			old.swap( mSlots );
			mSlots.resize( old.empty() ? 64 : old.size() * 2 );
			std::vector<size_t> used;
			used.swap( mUsed );
			for ( size_t slot : used ) {
				( *this )[old[slot].Key] = old[slot].Item;
			}
		}
	};

	/// Values in lists, one list for each position that has one: the lists of every position
	/// share two arrays instead of a container each, and keep the order of append().
	template <typename Value> class ListsByPosition {
	  public:
		static constexpr size_t None = static_cast<size_t>( -1 );

		void reset( size_t positions ) {
			const size_t none = None;
			mHead.assign( positions, none );
			mTail.assign( positions, none );
			mLinks.clear();
		}

		void append( size_t position, const Value& value ) {
			const size_t link = mLinks.size();
			const size_t none = None;
			mLinks.push_back( std::make_pair( value, none ) );
			if ( None == mTail[position] )
				mHead[position] = link;
			else
				mLinks[mTail[position]].second = link;
			mTail[position] = link;
		}

		/// Calls visit with the values of a position's list, in order.
		template <typename Visit> void forEach( size_t position, Visit visit ) const {
			for ( size_t link = mHead[position]; None != link; link = mLinks[link].second )
				visit( mLinks[link].first );
		}

		/// Empties a position's list.
		void drop( size_t position ) { mHead[position] = mTail[position] = None; }

	  private:
		std::vector<size_t> mHead;
		std::vector<size_t> mTail;
		std::vector<std::pair<Value, size_t>> mLinks;
	};

	/// A registration's events waiting for a move, in order, from Front on.
	struct WaitingEvents {
		std::vector<size_t> Events;
		size_t Front{ 0 };

		bool empty() const { return Front == Events.size(); }
	};

	enum class MoveState : unsigned char { Unresolved, Delivered, Broken };

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
		/// The directory move whose source contains a move's source, both by Add position: when
		/// its Moved event comes first, the move's old path is where that event put the entry.
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
	/// The events of the pass before this position were held by the previous one. Unlike
	/// mReleased, kept until the next pass.
	size_t mPassReleased{ 0 };

	/// Containers reused from one pass to the next, cleared before each use, so that a pass only
	/// allocates when it holds more than the ones before. They keep the memory of the largest
	/// pass since they were last released: see releaseScratch().
	static constexpr size_t RetainedEvents = 1024;
	struct Scratch {
		/// The halves seen for each identity of the pass.
		IdentityTable<Halves> HalvesSeen;
		std::vector<bool> Rescanned;
		// rescanBeforeMovedTrees().
		std::vector<const FileInfo*> Arrived;
		// destinationTree().
		std::unordered_map<std::string, const FileInfo*> TreeAtHold;
		std::vector<FileInfo> RenamedAtHold;
		std::vector<FileIdentity> ArrivedIdentities;
		/// Identities of unpaired Adds, sorted when looked up.
		std::vector<FileIdentity> Later;
		IdentityTable<Candidates> MoveCandidates;
		std::vector<std::pair<size_t, size_t>> Pairs;
		// followEnclosingMoves().
		std::vector<size_t> After;
		std::vector<size_t> FollowerStart;
		std::vector<size_t> Followers;
		std::vector<size_t> Order;
		std::vector<size_t> Stack;
		std::vector<size_t> Position;
		std::vector<size_t> Remapped;
		std::vector<Event> Events;
		// deliver().
		std::unordered_set<std::string> AddedTrees;
		std::unordered_set<std::string> DeletedTrees;
		std::vector<const FileInfo*> Contents;
		std::vector<MoveState> State;
		std::vector<bool> Converted;
		std::unordered_map<Watcher*, WaitingEvents> Waiting;
		size_t WaitingOwners{ 0 };
		std::vector<std::pair<const std::string*, size_t>> Sources;
		std::vector<size_t> SortedMoves;
		std::vector<size_t> ResolvedUntil;
		std::vector<std::pair<size_t, size_t>> MovesOut;
		std::unordered_map<std::string, std::string> CanonicalDirectories;
		ListsByPosition<Watcher*> BlockedBy;
		std::vector<Watcher*> Unblocked;
		std::vector<size_t> Rest;
		std::vector<bool> InRest;
		std::vector<bool> Unresolved;
		ListsByPosition<size_t> Skipped;
	};
	Scratch mScratch;
	Moves mMoves;

	/// The most events of a pass since the reused containers were last released.
	size_t mLargestPass{ 0 };

	/// Releases the reused containers, and the events' spare capacity, once a pass is much
	/// smaller than the largest one since, and that one held more than RetainedEvents: a burst
	/// does not keep its memory, and passes of a steady size reuse it.
	void releaseScratch( size_t events );

	void addHalves( size_t begin );

	/// @return The halves seen for an identity of the pass.
	const Halves& halvesOf( const FileIdentity& identity ) const;

	/// @return False when the batch has no move.
	bool matchMoves( Moves& moves );

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
	/// Sets, for each segment, whether its watch was scanned again, in mScratch.Rescanned.
	void rescanMoveSources();

	/// @return True when events were held.
	bool holdMoveSources( const Moves& moves );

	/// Appends the entries of a compared directory move's destination tree. A move held from the
	/// previous pass is compared with the tree as it was then: the Adds, Deletes and renames its
	/// watch collected inside it since are undone, since they are reported after its Moved event.
	/// @return False when the destination does not watch inside the directory.
	bool destinationTree( size_t move, std::vector<const FileInfo*>& entries );

	/// Scans again the watches scanned before a compared directory move whose tree lost
	/// entries: an entry that left it after their scan, to one of them, is found by the rescan.
	/// @return True when a watch was scanned again.
	bool rescanBeforeMovedTrees( const Moves& moves );

	/// @return Whether an entry of the tree a compared directory move left is neither in the tree
	/// at its destination nor added anywhere in the pass: it may have moved to a watch that was
	/// scanned before it left.
	bool movedTreeLostEntries( const Moves& moves, size_t move,
							   const std::unordered_map<std::string, FileIdentity>& roots );
};

} // namespace efsw

#endif
