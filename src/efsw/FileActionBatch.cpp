#include <algorithm>
#include <deque>
#include <efsw/FileActionBatch.hpp>
#include <efsw/FileSystem.hpp>
#include <iterator>
#include <unordered_map>

namespace efsw {

// Required before C++17, where Moves::None is odr-used by assign().
constexpr size_t FileActionBatch::Moves::None;

template <typename Value> class FileActionBatch::IdentityTable {
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

template <typename Value> class FileActionBatch::ListsByPosition {
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

struct FileActionBatch::Scratch {
	struct Candidates {
		size_t Added{ 0 }, Deleted{ 0 }, Created{ 0 }, Removed{ 0 };
	};
	enum class MoveState : unsigned char { Unresolved, Delivered, Broken };
	struct WaitingEvents {
		std::vector<size_t> Events;
		size_t Front{ 0 };
		bool empty() const { return Front == Events.size(); }
	};
	HalvesMap Halves;
	IdentityTable<Candidates> CandidatesByIdentity;
	Moves Matched;
	std::vector<bool> Rescanned, InRest, Unresolved;
	std::vector<FileIdentity> Later;
	std::unordered_map<Watcher*, size_t> HoldFrom;
	std::vector<Event> Kept, Events;
	std::vector<std::pair<size_t, size_t>> Pairs, Prerequisites;
	std::vector<size_t> After, FollowerStart, Followers, Order, Stack, Position, Remapped, Rest,
		SortedDependencies, ResolvedUntil;
	std::vector<std::pair<const std::string*, size_t>> Sources;
	std::unordered_map<std::string, std::vector<size_t>> AddsAt;
	std::vector<MoveState> State;
	std::unordered_map<Watcher*, WaitingEvents> Waiting;
	size_t WaitingOwners{ 0 };
	ListsByPosition<Watcher*> BlockedBy;
	ListsByPosition<size_t> Skipped;
	std::vector<Watcher*> Unblocked;
	std::unordered_map<std::string, std::string> CanonicalDirectories;
};

FileActionBatch::FileActionBatch() = default;
FileActionBatch::~FileActionBatch() = default;
FileActionBatch::FileActionBatch( FileActionBatch&& ) noexcept = default;
FileActionBatch& FileActionBatch::operator=( FileActionBatch&& ) noexcept = default;

FileActionBatch::Scratch& FileActionBatch::scratch() {
	if ( !mScratch ) {
		mScratch.reset( new Scratch );
	}
	return *mScratch;
}

void FileActionBatch::releaseScratch( size_t events ) {
	events += mHeld.size();
	mLargestPass = std::max( mLargestPass, events );
	if ( mLargestPass <= 1024 || 4 * events >= mLargestPass ) {
		return;
	}
	mScratch.reset();
	std::vector<Event>().swap( mEvents );
	std::vector<Segment>().swap( mSegments );
	if ( mHeld.empty() ) {
		std::vector<Event>().swap( mHeld );
	}
	mLargestPass = events;
}

FileActionBatch::Event::Event( WatchID watchid, const std::string& directory,
							   const std::string& filename, Action action,
							   const std::string& oldFilename, const FileInfo& fileInfo,
							   FileWatchListener* listener, Watcher* owner ) :
	Watch( watchid ),
	Directory( directory ),
	Filename( filename ),
	ActionType( action ),
	OldFilename( oldFilename ),
	Info( fileInfo ),
	Listener( listener ),
	Owner( owner ) {}

void FileActionBatch::clear() {
	mEvents.clear();
}

void FileActionBatch::add( WatchID watchid, const std::string& directory,
						   const std::string& filename, Action action,
						   const std::string& oldFilename, const FileInfo& fileInfo,
						   FileWatchListener* listener, Watcher* owner ) {
	mEvents.emplace_back( watchid, directory, filename, action, oldFilename, fileInfo, listener,
						  owner );
}

/// A directory may be reported with a final separator, but does not contain itself.
static size_t directoryLength( const std::string& path ) {
	return !path.empty() && path.back() == FileSystem::getOSSlash() ? path.size() - 1 : path.size();
}

/// The value of the deepest directory of a map that contains a path, by paths without a final
/// separator. Each directory is looked up once: the paths below it share its lookup.
template <typename Value> class NearestDirectory {
  public:
	NearestDirectory( const std::unordered_map<std::string, Value>& directories, Value none ) :
		mDirectories( directories ), mNone( none ) {}

	/// @return The value of the deepest directory of the map that contains the path, not the
	/// path itself, or none.
	Value of( const std::string& path ) {
		const size_t length = directoryLength( path );
		if ( 0 == length )
			return mNone;
		const size_t slash = path.rfind( FileSystem::getOSSlash(), length - 1 );
		if ( std::string::npos == slash || 0 == slash )
			return mNone;
		// The directories from the path's own up to the first one known.
		std::vector<std::string> unknown;
		Value found = mNone;
		for ( std::string directory = path.substr( 0, slash ); !directory.empty(); ) {
			auto known = mKnown.find( directory );
			if ( known != mKnown.end() ) {
				found = known->second;
				break;
			}
			auto listed = mDirectories.find( directory );
			if ( listed != mDirectories.end() ) {
				found = listed->second;
				mKnown[directory] = found;
				break;
			}
			unknown.push_back( directory );
			const size_t parent = directory.rfind( FileSystem::getOSSlash() );
			if ( std::string::npos == parent || 0 == parent )
				break;
			directory.resize( parent );
		}
		for ( const std::string& directory : unknown )
			mKnown[directory] = found;
		return found;
	}

  private:
	const std::unordered_map<std::string, Value>& mDirectories;
	Value mNone;
	std::unordered_map<std::string, Value> mKnown;
};

static bool metadataMatches( const FileInfo& source, const FileInfo& destination ) {
	if ( source.isDirectory() != destination.isDirectory() ||
		 source.isRegularFile() != destination.isRegularFile() ||
		 source.OwnerId != destination.OwnerId || source.GroupId != destination.GroupId ||
		 source.Permissions != destination.Permissions )
		return false;
	// A directory's size and modification time change whenever its entries change, and
	// moving it to another parent rewrites its ".." entry on some filesystems. Directories
	// cannot be hard-linked, so the device and inode identity alone is unambiguous.
	if ( source.isDirectory() )
		return true;
	return source.ModificationTime == destination.ModificationTime &&
		   source.Size == destination.Size;
}

void FileActionBatch::beginPass() {
	mSegments.clear();
	mHeld.erase( std::remove_if( mHeld.begin(), mHeld.end(),
								 []( const Event& event ) { return event.Canceled; } ),
				 mHeld.end() );
	for ( Event& event : mHeld )
		event.PreviousPass = true;
	mEvents.insert( mEvents.begin(), std::make_move_iterator( mHeld.begin() ),
					std::make_move_iterator( mHeld.end() ) );
	mHeld.clear();
	mReleased = mEvents.size();
}

void FileActionBatch::addSegment( Watcher* owner, size_t begin ) {
	for ( size_t i = begin; i < mEvents.size(); i++ )
		if ( NULL == mEvents[i].Owner )
			mEvents[i].Owner = owner;
	mSegments.push_back( { owner, begin, mEvents.size() } );
}

void FileActionBatch::scanned( Watcher* watcher ) {
	if ( !watcher->collectsSharedActions() )
		return;
	addSegment( watcher, mSegments.empty() ? mReleased : mSegments.back().End );
}

void FileActionBatch::cancel() {
	for ( Event& event : mEvents )
		event.Canceled = true;
}

void FileActionBatch::forget( Watcher* watcher ) {
	// Callback cancellation must not invalidate positions or references during dispatch.
	for ( Event& event : mHeld ) {
		if ( event.Owner == watcher )
			event.Canceled = true;
	}
	// A dispatch may be iterating mEvents: flag the events instead of erasing them.
	for ( Event& event : mEvents ) {
		if ( event.Owner == watcher )
			event.Canceled = true;
	}
	for ( Segment& segment : mSegments ) {
		if ( segment.Owner == watcher )
			segment.Owner = NULL;
	}
}

void FileActionBatch::addHalves( HalvesMap& halves, size_t begin ) const {
	for ( size_t i = begin; i < mEvents.size(); i++ ) {
		const Event& event = mEvents[i];
		if ( event.Info.Inode == 0 )
			continue;
		if ( event.ActionType == Actions::Add )
			halves[event.Info.identity()].Added = true;
		else if ( event.ActionType == Actions::Delete )
			halves[event.Info.identity()].Deleted = true;
	}
}

const std::vector<bool>& FileActionBatch::rescanMoveSources( HalvesMap& halves ) {
	const size_t count = mSegments.size();
	std::vector<bool>& rescanned = scratch().Rescanned;
	rescanned.assign( count, false );
	// Identities of the unpaired Adds collected after the segment being considered.
	std::vector<FileIdentity>& later = scratch().Later;
	later.clear();
	for ( size_t s = count; s-- > 0; ) {
		later.erase( std::remove_if( later.begin(), later.end(),
									 [&halves]( const FileIdentity& identity ) {
										 const Halves* known = halves.find( identity );
										 return known && known->paired();
									 } ),
					 later.end() );
		// Copied, a rescan adds a segment.
		const Segment segment = mSegments[s];
		if ( segment.Owner && !later.empty() && segment.Owner->snapshotHasAny( later ) ) {
			size_t begin = mEvents.size();
			segment.Owner->watch();
			addHalves( halves, begin );
			addSegment( segment.Owner, begin );
			rescanned[s] = true;
		}
		const size_t known = later.size();
		for ( size_t i = segment.Begin; i < segment.End; i++ ) {
			const Event& event = mEvents[i];
			if ( event.ActionType != Actions::Add || event.Info.Inode == 0 )
				continue;
			if ( !halves[event.Info.identity()].paired() )
				later.push_back( event.Info.identity() );
		}
		if ( later.size() != known ) {
			std::sort( later.begin(), later.end() );
			later.erase( std::unique( later.begin(), later.end() ), later.end() );
		}
	}
	return rescanned;
}

bool FileActionBatch::holdMoveSources( const HalvesMap& halves, const std::vector<bool>& rescanned,
									   const Moves& moves ) {
	// For each watch, the position of its first held event.
	auto& holdFrom = scratch().HoldFrom;
	holdFrom.clear();
	// Whether a watch scanned before the segment was not scanned again since. When the
	// segment has a Delete, that watch may only see the destination on its next scan.
	bool canWait = false;
	for ( size_t s = 0; s < rescanned.size(); s++ ) {
		const Segment& segment = mSegments[s];
		for ( size_t i = segment.Begin; canWait && segment.Owner && i < segment.End; i++ ) {
			const Event& event = mEvents[i];
			if ( event.ActionType == Actions::Delete && event.Info.Inode != 0 &&
				 moves.MoveOf[i] == Moves::None &&
				 !halves.find( event.Info.identity() )->paired() ) {
				holdFrom.emplace( segment.Owner, i );
				break;
			}
		}
		if ( !rescanned[s] )
			canWait = true;
	}
	if ( holdFrom.empty() )
		return false;

	// Later events of a held watch are held too, including those of its rescan.
	auto inHeldRange = [&]( size_t i ) {
		auto hold = i < mReleased ? holdFrom.end() : holdFrom.find( mEvents[i].Owner );
		return hold != holdFrom.end() && i >= hold->second;
	};
	// A complete move follows its destination, where its Moved event is delivered. Held, it
	// still pairs on the next pass. A released event can be held once more this way, but
	// not after: its destination is then released too.
	std::vector<Event>& kept = scratch().Kept;
	kept.clear();
	kept.reserve( mEvents.size() );
	for ( size_t i = 0; i < mEvents.size(); i++ ) {
		size_t move = moves.MoveOf[i];
		if ( inHeldRange( move == Moves::None ? i : move ) )
			mHeld.push_back( std::move( mEvents[i] ) );
		else
			kept.push_back( std::move( mEvents[i] ) );
	}
	mEvents.swap( kept );
	kept.clear();
	return true;
}

void FileActionBatch::finishPass() {
	if ( mEvents.empty() ) {
		mSegments.clear();
		mReleased = 0;
		releaseScratch( 0 );
		return;
	}
	const size_t events = mEvents.size();
	Scratch& work = scratch();
	HalvesMap& halves = work.Halves;
	halves.clear();
	addHalves( halves, 0 );
	bool unpaired = false;
	halves.forEach(
		[&unpaired]( const Halves& identity ) { unpaired = unpaired || !identity.paired(); } );
	const std::vector<bool>& rescanned = unpaired ? rescanMoveSources( halves ) : work.Rescanned;

	Moves& moves = work.Matched;
	bool hasMoves = matchMoves( moves );
	if ( unpaired && holdMoveSources( halves, rescanned, moves ) )
		hasMoves = matchMoves( moves );
	mSegments.clear();
	mReleased = 0;
	if ( hasMoves )
		followEnclosingMoves( moves );
	deliver( NULL, hasMoves ? &moves : NULL );
	releaseScratch( events );
}

bool FileActionBatch::matchMoves( Moves& moves ) {
	Scratch& work = scratch();
	moves.MoveOf.assign( mEvents.size(), Moves::None );
	moves.SourceOf.assign( mEvents.size(), Moves::None );
	bool hasAdd = false;
	bool hasDelete = false;
	for ( const Event& event : mEvents ) {
		if ( event.Info.Inode == 0 )
			continue;
		hasAdd |= event.ActionType == Actions::Add;
		hasDelete |= event.ActionType == Actions::Delete;
		if ( hasAdd && hasDelete )
			break;
	}
	if ( !hasAdd || !hasDelete )
		return false;

	auto& candidates = work.CandidatesByIdentity;
	candidates.clear();
	for ( size_t i = 0; i < mEvents.size(); i++ ) {
		const Event& event = mEvents[i];
		if ( event.Info.Inode == 0 ||
			 ( event.ActionType != Actions::Add && event.ActionType != Actions::Delete ) )
			continue;

		Scratch::Candidates& identity = candidates[event.Info.identity()];
		if ( event.ActionType == Actions::Add ) {
			identity.Added++;
			identity.Created = i;
		} else {
			identity.Deleted++;
			identity.Removed = i;
		}
	}

	// The Add and Delete positions of each identity with one of each. Directories come first,
	// outermost first, by destination path: a moved directory claims the events inside it
	// before the moves nested in it are considered. Then the rest, in collection order.
	std::vector<std::pair<size_t, size_t>>& pairs = work.Pairs;
	pairs.clear();
	candidates.forEach( [&pairs]( const Scratch::Candidates& entries ) {
		if ( entries.Added == 1 && entries.Deleted == 1 ) {
			pairs.emplace_back( entries.Created, entries.Removed );
		}
	} );
	std::sort( pairs.begin(), pairs.end(),
			   [&]( const std::pair<size_t, size_t>& a, const std::pair<size_t, size_t>& b ) {
				   const bool aDirectory = mEvents[a.second].Info.isDirectory();
				   const bool bDirectory = mEvents[b.second].Info.isDirectory();
				   if ( aDirectory != bDirectory )
					   return aDirectory;
				   if ( aDirectory ) {
					   const int order =
						   mEvents[a.first].Info.Filepath.compare( mEvents[b.first].Info.Filepath );
					   if ( order != 0 )
						   return order < 0;
				   }
				   return a.first < b.first;
			   } );

	std::unordered_map<std::string, size_t> sourceRoots, destinationRoots;
	NearestDirectory<size_t> sourceDirectories( sourceRoots, Moves::None );
	NearestDirectory<size_t> destinationDirectories( destinationRoots, Moves::None );
	// Directory candidates update these maps outermost first. Once all directories have
	// been considered, file candidates can share a memoized lookup of their parents.
	auto enclosing = []( const std::unordered_map<std::string, size_t>& roots,
						 const std::string& path ) {
		if ( roots.empty() )
			return Moves::None;
		const size_t length = directoryLength( path );
		for ( size_t slash = length ? path.rfind( FileSystem::getOSSlash(), length - 1 )
									: std::string::npos;
			  slash != std::string::npos && slash > 0;
			  slash = path.rfind( FileSystem::getOSSlash(), slash - 1 ) ) {
			auto found = roots.find( path.substr( 0, slash ) );
			if ( found != roots.end() )
				return found->second;
		}
		return Moves::None;
	};

	// Whether an entry keeps its path relative to the directory move that encloses both of its
	// halves: then that move's Moved event reports it. A rename or relocation inside the moved
	// tree does not, and is a move of its own.
	auto keepsRelativePath = [&]( size_t enclosing, size_t deleted, size_t created ) {
		// Lengths of the enclosing move's paths, without a trailing separator.
		auto length = []( const std::string& directory ) {
			return directory.empty() || directory.back() != FileSystem::getOSSlash()
					   ? directory.size()
					   : directory.size() - 1;
		};
		const size_t sourceLength = length( mEvents[moves.SourceOf[enclosing]].Info.Filepath );
		const size_t destinationLength = length( mEvents[enclosing].Info.Filepath );
		return mEvents[deleted].Info.Filepath.compare( sourceLength, std::string::npos,
													   mEvents[created].Info.Filepath,
													   destinationLength, std::string::npos ) == 0;
	};

	bool hasMove = false;
	auto& addsAt = work.AddsAt;
	addsAt.clear();
	bool addsIndexed = false;
	auto sourceTaken = [&]( size_t deleted, size_t created ) {
		const Event& source = mEvents[deleted];
		if ( NULL == source.Owner || source.Owner != mEvents[created].Owner )
			return false;
		if ( !addsIndexed ) {
			addsIndexed = true;
			for ( size_t i = 0; i < mEvents.size(); ++i ) {
				if ( Actions::Add == mEvents[i].ActionType && NULL != mEvents[i].Owner ) {
					addsAt[mEvents[i].Info.Filepath.substr(
							   0, directoryLength( mEvents[i].Info.Filepath ) )]
						.push_back( i );
				}
			}
		}
		auto found = addsAt.find(
			source.Info.Filepath.substr( 0, directoryLength( source.Info.Filepath ) ) );
		if ( found == addsAt.end() )
			return false;
		for ( size_t add : found->second ) {
			if ( add < created && mEvents[add].Owner == source.Owner )
				return true;
		}
		return false;
	};
	for ( const auto& pair : pairs ) {
		const size_t created = pair.first;
		const size_t deleted = pair.second;
		const bool directory = mEvents[deleted].Info.isDirectory();
		const size_t enclosingSource =
			sourceRoots.empty() ? Moves::None
			: directory			? enclosing( sourceRoots, mEvents[deleted].Info.Filepath )
								: sourceDirectories.of( mEvents[deleted].Info.Filepath );
		const size_t enclosingDestination =
			destinationRoots.empty() ? Moves::None
			: directory				 ? enclosing( destinationRoots, mEvents[created].Info.Filepath )
									 : destinationDirectories.of( mEvents[created].Info.Filepath );
		if ( Moves::None != enclosingSource && enclosingSource == enclosingDestination &&
			 keepsRelativePath( enclosingSource, deleted, created ) )
			continue;
		const Event& source = mEvents[deleted];
		const Event& destination = mEvents[created];
		const size_t sourceLength = directoryLength( source.Info.Filepath );
		if ( ( sourceLength == directoryLength( destination.Info.Filepath ) &&
			   0 == source.Info.Filepath.compare( 0, sourceLength, destination.Info.Filepath, 0,
												  sourceLength ) ) ||
			 !source.Info.sameInode( destination.Info ) ||
			 ( source.Info.isRegularFile() &&
			   ( source.Info.LinkCount != 1 || destination.Info.LinkCount != 1 ) ) ||
			 !metadataMatches( source.Info, destination.Info ) || sourceTaken( deleted, created ) )
			continue;

		moves.MoveOf[deleted] = created;
		moves.MoveOf[created] = created;
		moves.SourceOf[created] = deleted;
		hasMove = true;

		if ( source.Info.isDirectory() ) {
			sourceRoots[source.Info.Filepath.substr( 0, directoryLength( source.Info.Filepath ) )] =
				created;
			destinationRoots[destination.Info.Filepath.substr(
				0, directoryLength( destination.Info.Filepath ) )] = created;
		}
	}
	if ( !sourceRoots.empty() ) {
		// Each event belongs to its nearest moved ancestor, unless already a move of its
		// own. This does not walk every descendant again for each nested directory move.
		for ( size_t i = 0; i < mEvents.size(); ++i ) {
			const Event& event = mEvents[i];
			if ( moves.MoveOf[i] != Moves::None )
				continue;
			const bool deleted = event.ActionType == Actions::Delete;
			if ( !deleted && event.ActionType != Actions::Add )
				continue;
			const size_t move = deleted ? sourceDirectories.of( event.Info.Filepath )
										: destinationDirectories.of( event.Info.Filepath );
			if ( move != Moves::None &&
				 event.Owner == mEvents[deleted ? moves.SourceOf[move] : move].Owner ) {
				// Earlier source changes must stay before this pass's parent move; later
				// destination arrivals must stay after a move released from an earlier pass.
				if ( deleted && event.PreviousPass &&
					 !mEvents[moves.SourceOf[move]].PreviousPass ) {
					continue;
				}
				if ( !deleted && !event.PreviousPass && mEvents[move].PreviousPass ) {
					continue;
				}
				moves.MoveOf[i] = move;
			}
		}
	}
	return hasMove;
}

void FileActionBatch::followEnclosingMoves( Moves& moves ) {
	Scratch& work = scratch();
	const size_t count = mEvents.size();
	// The directory moves by their destination, each with the one enclosing it.
	std::unordered_map<std::string, size_t> destinations;
	for ( size_t move = 0; move < count; move++ )
		if ( moves.MoveOf[move] == move && mEvents[move].Info.isDirectory() )
			destinations[mEvents[move].Info.Filepath.substr(
				0, directoryLength( mEvents[move].Info.Filepath ) )] = move;
	if ( destinations.empty() )
		return;
	NearestDirectory<size_t> enclosing( destinations, Moves::None );
	std::unordered_map<size_t, size_t> outer;
	for ( const auto& destination : destinations )
		outer[destination.second] = enclosing.of( mEvents[destination.second].Info.Filepath );

	// For each event, the deepest directory move after it whose destination contains it. That
	// move follows the moves enclosing it the same way, so all of them come first. Deletes
	// refer to what was there before: they are not moved past the directory's arrival.
	std::vector<size_t>& after = work.After;
	after.assign( count, Moves::None );
	bool reorders = false;
	for ( size_t i = 0; i < count; i++ ) {
		if ( mEvents[i].ActionType == Actions::Delete || mEvents[i].Info.Filepath.empty() )
			continue;
		for ( size_t move = enclosing.of( mEvents[i].Info.Filepath ); Moves::None != move;
			  move = outer.at( move ) ) {
			if ( move > i ) {
				after[i] = move;
				reorders = true;
				break;
			}
		}
	}
	if ( !reorders )
		return;

	// Each event that follows a move goes right after it, in collection order, each followed by
	// its own followers. A move is always later than the events that follow it, so this ends.
	// The followers of move m are Followers[FollowerStart[m] .. FollowerStart[m + 1]).
	std::vector<size_t>& start = work.FollowerStart;
	std::vector<size_t>& followers = work.Followers;
	start.assign( count + 1, 0 );
	for ( size_t i = 0; i < count; i++ )
		if ( Moves::None != after[i] )
			start[after[i] + 1]++;
	for ( size_t m = 0; m < count; m++ )
		start[m + 1] += start[m];
	followers.resize( start[count] );
	{
		// Filled in collection order: the next free place of each move's followers.
		std::vector<size_t>& next = work.Position;
		next.assign( start.begin(), start.end() - 1 );
		for ( size_t i = 0; i < count; i++ )
			if ( Moves::None != after[i] )
				followers[next[after[i]]++] = i;
	}
	std::vector<size_t>& order = work.Order;
	order.clear();
	order.reserve( count );
	std::vector<size_t>& stack = work.Stack;
	stack.clear();
	for ( size_t i = 0; i < count; i++ ) {
		if ( Moves::None != after[i] )
			continue;
		stack.push_back( i );
		while ( !stack.empty() ) {
			const size_t next = stack.back();
			stack.pop_back();
			order.push_back( next );
			for ( size_t f = start[next + 1]; f-- > start[next]; )
				stack.push_back( followers[f] );
		}
	}

	std::vector<size_t>& position = work.Position;
	position.resize( count );
	for ( size_t i = 0; i < count; i++ )
		position[order[i]] = i;
	std::vector<size_t>& remapped = work.Remapped;
	auto remap = [&]( std::vector<size_t>& links ) {
		remapped.assign( count, Moves::None );
		for ( size_t i = 0; i < count; i++ )
			if ( Moves::None != links[i] )
				remapped[position[i]] = position[links[i]];
		links.swap( remapped );
	};
	remap( moves.MoveOf );
	remap( moves.SourceOf );
	std::vector<Event>& events = work.Events;
	events.clear();
	events.reserve( count );
	for ( size_t i : order )
		events.push_back( std::move( mEvents[i] ) );
	mEvents.swap( events );
	events.clear();
}

void FileActionBatch::deliverEvent( FileWatchListener* listener, size_t index ) {
	const Event& event = mEvents[index];
	if ( event.Canceled )
		return;
	FileWatchListener* target = event.Listener ? event.Listener : listener;
	if ( target )
		target->handleFileAction( event.Watch, event.Directory, event.Filename, event.ActionType,
								  event.OldFilename );
}

void FileActionBatch::deliver( FileWatchListener* listener, const Moves* moves ) {
	// Indexed, and the flags read at each step: a listener may cancel events of a removed watch.
	if ( !moves ) {
		for ( size_t i = 0; i < mEvents.size(); i++ )
			deliverEvent( listener, i );
		clear();
		return;
	}

	// A move is delivered as a Moved event at its destination's position. Its source events,
	// the Delete and those inside a moved directory, are reported by it, unless a listener
	// removes the destination's registration before then: the move falls apart, and the source
	// events are delivered on their own. Until the move is resolved, the later events of the
	// source's registration wait behind them, so that every registration keeps its order: a
	// Delete must not follow the Add of a new file at the same path. A move whose source is
	// removed is still delivered: it happened while both were registered.
	Scratch& work = scratch();
	typedef Scratch::MoveState MoveState;
	typedef Scratch::WaitingEvents WaitingEvents;
	std::vector<MoveState>& state = work.State;
	state.assign( mEvents.size(), MoveState::Unresolved );

	// The events waiting for a move, by registration. The registrations stay, with no event,
	// from one delivery to the next; a listener may remove one meanwhile, which must not
	// invalidate the queue being emptied. Their number is bounded by clearing them all.
	std::unordered_map<Watcher*, WaitingEvents>& waiting = work.Waiting;
	if ( waiting.size() > 64 )
		waiting.clear();
	for ( auto& queue : waiting ) {
		queue.second.Events.clear();
		queue.second.Front = 0;
	}
	size_t& waitingOwners = work.WaitingOwners;
	waitingOwners = 0;

	// A directory's Delete waits for the moves out of it, or a listener would no longer find
	// their sources. A directory move waits for independent Deletes inside its source tree:
	// a released child Delete may itself wait for a file move, and must precede its parent's
	// Moved event even when another queued event hides that Delete at the source watch.
	// Two path-sorted ranges share the same arrays: independent directory Deletes, then moves
	// by source path. Nested directories share ranges instead of copying dependencies per depth.
	std::vector<size_t>& dependencies = work.SortedDependencies;
	dependencies.clear();
	// For each position, one at or after it such that every prerequisite in between is resolved.
	std::vector<size_t>& resolvedUntil = work.ResolvedUntil;
	resolvedUntil.clear();
	// By event, its prerequisite range in dependencies, or None.
	std::vector<std::pair<size_t, size_t>>& prerequisites = work.Prerequisites;
	prerequisites.clear();
	bool deletesDirectories = false;
	for ( size_t i = 0; i < mEvents.size() && !deletesDirectories; i++ )
		deletesDirectories = Actions::Delete == mEvents[i].ActionType &&
							 Moves::None == moves->MoveOf[i] && mEvents[i].Info.isDirectory();
	if ( deletesDirectories ) {
		std::vector<std::pair<const std::string*, size_t>>& sources = work.Sources;
		sources.clear();
		for ( size_t i = 0; i < mEvents.size(); i++ )
			if ( Actions::Delete == mEvents[i].ActionType && Moves::None == moves->MoveOf[i] &&
				 mEvents[i].Info.isDirectory() )
				sources.emplace_back( &mEvents[i].Info.Filepath, i );
		const size_t deletes = sources.size();
		for ( size_t i = 0; i < mEvents.size(); i++ )
			if ( moves->MoveOf[i] == i )
				sources.emplace_back( &mEvents[moves->SourceOf[i]].Info.Filepath, i );
		prerequisites.assign( mEvents.size(), std::make_pair( Moves::None, Moves::None ) );
		auto byPath = []( const std::pair<const std::string*, size_t>& a,
						  const std::pair<const std::string*, size_t>& b ) {
			return *a.first < *b.first;
		};
		std::sort( sources.begin(), sources.begin() + deletes, byPath );
		std::sort( sources.begin() + deletes, sources.end(), byPath );
		dependencies.reserve( sources.size() );
		for ( const auto& source : sources )
			dependencies.push_back( source.second );
		resolvedUntil.resize( dependencies.size() );
		for ( size_t position = 0; position < resolvedUntil.size(); position++ )
			resolvedUntil[position] = position;
		// Search for the virtual key "directory + separator" without allocating it. The
		// separator's successor is the upper bound, excluding prefix siblings such as dir-x.
		auto firstFrom = [&sources]( const std::string& directory, unsigned char separator,
									 size_t begin, size_t end ) {
			const size_t length = directoryLength( directory );
			return static_cast<size_t>(
				std::lower_bound(
					sources.begin() + begin, sources.begin() + end, directory,
					[length, separator]( const std::pair<const std::string*, size_t>& entry,
										 const std::string& value ) {
						const int compared = entry.first->compare( 0, length, value, 0, length );
						return compared < 0 ||
							   ( compared == 0 && ( entry.first->size() == length ||
													static_cast<unsigned char>(
														( *entry.first )[length] ) < separator ) );
					} ) -
				sources.begin() );
		};
		for ( size_t i = 0; i < mEvents.size(); i++ ) {
			const Event& event = mEvents[i];
			const bool deleted =
				Actions::Delete == event.ActionType && Moves::None == moves->MoveOf[i];
			if ( !event.Info.isDirectory() || ( !deleted && moves->MoveOf[i] != i ) )
				continue;
			// The paths below the directory start with it and a separator: they sort from
			// there up to the same path with the separator's successor.
			const std::string& path =
				deleted ? event.Info.Filepath : mEvents[moves->SourceOf[i]].Info.Filepath;
			const size_t begin = deleted ? deletes : 0;
			const size_t end = deleted ? sources.size() : deletes;
			const unsigned char separator = FileSystem::getOSSlash();
			const size_t first = firstFrom( path, separator, begin, end );
			const size_t last = firstFrom( path, separator + 1, begin, end );
			if ( first < last )
				prerequisites[i] = std::make_pair( first, last );
		}
	}
	// Resolved entries are skipped and path-compressed for later lookups of shared ranges.
	// @return The position of the first prerequisite that may be unresolved.
	auto firstUnresolved = [&]( size_t position ) {
		size_t found = position;
		while ( found < dependencies.size() && MoveState::Unresolved != state[dependencies[found]] )
			found = std::max( resolvedUntil[found], found + 1 );
		for ( size_t at = position; at < found; ) {
			const size_t after = std::max( resolvedUntil[at], at + 1 );
			resolvedUntil[at] = found;
			at = after;
		}
		return found;
	};

	// Whether a source event of a move waits for its destination. Not when both belong to one
	// registration, which is removed with both, or when the move is resolved.
	auto blocker = [&]( size_t i ) {
		if ( !prerequisites.empty() && Moves::None != prerequisites[i].first ) {
			const size_t position = firstUnresolved( prerequisites[i].first );
			if ( position < prerequisites[i].second ) {
				return dependencies[position];
			}
		}
		const size_t move = moves->MoveOf[i];
		return Moves::None != move && move != i && mEvents[move].Owner != mEvents[i].Owner &&
					   MoveState::Unresolved == state[move]
				   ? move
				   : Moves::None;
	};

	// The absolute source of a Moved event. Resolving it climbs the source's directory until a
	// directory that still exists, as deep as a deleted tree goes: each directory is resolved once,
	// and the moves out of it share the result.
	std::unordered_map<std::string, std::string>& canonicalDirectories = work.CanonicalDirectories;
	canonicalDirectories.clear();
	auto canonicalSource = [&canonicalDirectories]( const std::string& path ) {
		const std::string name( FileSystem::fileNameFromPath( path ) );
		// A path with a final separator does not end with its name: resolved on its own.
		if ( name.empty() || path.size() < name.size() ||
			 0 != path.compare( path.size() - name.size(), name.size(), name ) )
			return FileSystem::canonicalSourcePath( path );
		const std::string directory( path.substr( 0, path.size() - name.size() ) );
		auto known = canonicalDirectories.find( directory );
		if ( known != canonicalDirectories.end() )
			return known->second + name;
		const std::string canonical( FileSystem::canonicalSourcePath( path ) );
		// The name is kept at the end: what comes before it is the directory's.
		canonicalDirectories.emplace( directory,
									  canonical.substr( 0, canonical.size() - name.size() ) );
		return canonical;
	};

	auto process = [&]( size_t i ) {
		const size_t move = moves->MoveOf[i];
		if ( Moves::None == move ) {
			deliverEvent( listener, i );
			state[i] = MoveState::Delivered;
		} else if ( move != i ) {
			if ( MoveState::Broken == state[move] )
				deliverEvent( listener, i );
		} else if ( mEvents[i].Canceled ) {
			state[i] = MoveState::Broken;
		} else {
			state[i] = MoveState::Delivered;
			const Event& destination = mEvents[i];
			FileWatchListener* target = destination.Listener ? destination.Listener : listener;
			if ( target ) {
				const Event& source = mEvents[moves->SourceOf[i]];
				target->handleFileAction(
					destination.Watch, destination.Directory, destination.Filename, Actions::Moved,
					source.Watch == destination.Watch && source.Directory == destination.Directory
						? source.Filename
						: canonicalSource( source.Info.Filepath ) );
			}
		}
	};

	// The registrations whose waiting events start with one that waits for an event, by it.
	ListsByPosition<Watcher*>& blockedBy = work.BlockedBy;
	blockedBy.reset( mEvents.size() );

	// An event was resolved: delivers the waiting events that no longer wait, those of the
	// registrations it blocked, and of those the events resolved meanwhile blocked.
	auto release = [&]( size_t resolved ) {
		std::vector<Watcher*>& unblocked = work.Unblocked;
		unblocked.clear();
		auto wake = [&]( size_t move ) {
			blockedBy.forEach( move,
							   [&unblocked]( Watcher* owner ) { unblocked.push_back( owner ); } );
			blockedBy.drop( move );
		};
		wake( resolved );
		while ( !unblocked.empty() ) {
			Watcher* owner = unblocked.back();
			unblocked.pop_back();
			auto it = waiting.find( owner );
			if ( it == waiting.end() || it->second.empty() )
				continue;
			WaitingEvents& queue = it->second;
			size_t move = Moves::None;
			while ( !queue.empty() &&
					Moves::None == ( move = blocker( queue.Events[queue.Front] ) ) ) {
				const size_t i = queue.Events[queue.Front++];
				process( i );
				if ( MoveState::Unresolved != state[i] )
					wake( i );
			}
			if ( queue.empty() ) {
				queue.Events.clear();
				queue.Front = 0;
				waitingOwners--;
			} else {
				blockedBy.append( move, owner );
			}
		}
	};

	for ( size_t i = 0; i < mEvents.size(); i++ ) {
		if ( waitingOwners > 0 ) {
			auto queue = waiting.find( mEvents[i].Owner );
			if ( queue != waiting.end() && !queue->second.empty() ) {
				queue->second.Events.push_back( i );
				continue;
			}
		}
		const size_t move = blocker( i );
		if ( Moves::None != move ) {
			WaitingEvents& queue = waiting[mEvents[i].Owner];
			queue.Events.push_back( i );
			waitingOwners++;
			blockedBy.append( move, mEvents[i].Owner );
			continue;
		}
		process( i );
		if ( waitingOwners > 0 && MoveState::Unresolved != state[i] )
			release( i );
	}

	// Moves whose registrations wait on each other, as crossed moves between two registrations
	// do, are never resolved: they are delivered as ordinary events, in collection order, so
	// that no source event is skipped for a destination that a later callback may still remove.
	// The events of such a destination's registration inside a moved directory, which the loop
	// above skipped, follow the directory's Add, which they come after.
	if ( waitingOwners > 0 ) {
		std::vector<size_t>& rest = work.Rest;
		rest.clear();
		for ( const auto& queue : waiting )
			rest.insert( rest.end(), queue.second.Events.begin() + queue.second.Front,
						 queue.second.Events.end() );
		std::sort( rest.begin(), rest.end() );
		std::vector<bool>& inRest = work.InRest;
		inRest.assign( mEvents.size(), false );
		std::vector<bool>& unresolved = work.Unresolved;
		unresolved.assign( mEvents.size(), false );
		for ( size_t i : rest ) {
			inRest[i] = true;
			const size_t move = moves->MoveOf[i];
			if ( Moves::None != move && move != i && MoveState::Unresolved == state[move] )
				unresolved[move] = true;
		}
		// The skipped events of each unresolved destination's registration, in order.
		ListsByPosition<size_t>& skipped = work.Skipped;
		skipped.reset( mEvents.size() );
		for ( size_t j = 0; j < mEvents.size(); j++ ) {
			const size_t move = moves->MoveOf[j];
			if ( Moves::None != move && move != j && unresolved[move] && !inRest[j] &&
				 mEvents[j].Owner == mEvents[move].Owner )
				skipped.append( move, j );
		}
		for ( size_t i : rest ) {
			const size_t move = moves->MoveOf[i];
			if ( Moves::None == move || !unresolved[move] ) {
				process( i );
				continue;
			}
			if ( move != i ) {
				deliverEvent( listener, i );
				state[move] = MoveState::Broken;
				continue;
			}
			state[i] = MoveState::Broken;
			deliverEvent( listener, i );
			skipped.forEach( i, [&]( size_t j ) { deliverEvent( listener, j ); } );
		}
	}
	clear();
}

void FileActionBatch::dispatch( FileWatchListener* listener, bool detectMoves ) {
	const size_t events = mEvents.size();
	if ( !detectMoves || mEvents.empty() ) {
		deliver( listener, NULL );
		releaseScratch( events );
		return;
	}
	Moves& moves = scratch().Matched;
	bool hasMoves = detectMoves && matchMoves( moves );
	if ( hasMoves )
		followEnclosingMoves( moves );
	deliver( listener, hasMoves ? &moves : NULL );
	releaseScratch( events );
}

} // namespace efsw
