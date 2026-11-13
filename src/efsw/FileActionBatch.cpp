#include <algorithm>
#include <cstring>
#include <deque>
#include <efsw/FileActionBatch.hpp>
#include <efsw/FileSystem.hpp>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace efsw {

// Required before C++17, where Moves::None is odr-used by assign().
constexpr size_t FileActionBatch::Moves::None;

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

/// The length of a directory path without its final separator.
static size_t directoryLength( const std::string& directory ) {
	return !directory.empty() && directory.back() == FileSystem::getOSSlash() ? directory.size() - 1
																			  : directory.size();
}

/// @return The value of the directory among roots, by path without a final separator, that
/// contains the path; or none.
template <typename Value>
static Value enclosingRoot( const std::unordered_map<std::string, Value>& roots,
							const std::string& path, Value none ) {
	if ( roots.empty() )
		return none;
	for ( size_t slash = path.rfind( FileSystem::getOSSlash() );
		  slash != std::string::npos && slash > 0;
		  slash = path.rfind( FileSystem::getOSSlash(), slash - 1 ) ) {
		auto root = roots.find( path.substr( 0, slash ) );
		if ( root != roots.end() )
			return root->second;
	}
	return none;
}

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
	// A dispatch may be iterating mEvents, and the changes of a moved tree point into mHeld:
	// flag the events instead of erasing them. The next pass drops the held ones.
	for ( Event& event : mHeld ) {
		if ( event.Owner == watcher )
			event.Canceled = true;
	}
	for ( Event& event : mEvents ) {
		if ( event.Owner == watcher )
			event.Canceled = true;
	}
	for ( Segment& segment : mSegments ) {
		if ( segment.Owner == watcher )
			segment.Owner = NULL;
	}
}

void FileActionBatch::addSourceTree( const FileInfo& directory, std::vector<FileInfoList> lists ) {
	if ( directory.Inode != 0 )
		mSourceTrees[directory.identity()] = std::move( lists );
}

void FileActionBatch::pruneSourceTrees() {
	if ( mSourceTrees.empty() )
		return;
	std::unordered_map<FileIdentity, std::vector<FileInfoList>, FileIdentityHash> kept;
	for ( const Event& event : mHeld ) {
		if ( event.ActionType != Actions::Delete || event.Canceled )
			continue;
		auto tree = mSourceTrees.find( event.Info.identity() );
		if ( tree != mSourceTrees.end() )
			kept.emplace( tree->first, std::move( tree->second ) );
	}
	mSourceTrees.swap( kept );
}

std::unordered_map<std::string, FileIdentity> FileActionBatch::evidenceRoots() const {
	std::unordered_map<std::string, FileIdentity> roots;
	if ( mSourceTrees.empty() )
		return roots;
	for ( const Event& event : mEvents )
		if ( Actions::Delete == event.ActionType && event.Info.Inode != 0 &&
			 mSourceTrees.count( event.Info.identity() ) )
			roots[event.Info.Filepath.substr( 0, directoryLength( event.Info.Filepath ) )] =
				event.Info.identity();
	return roots;
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

std::vector<bool> FileActionBatch::rescanMoveSources( HalvesMap& halves ) {
	const size_t count = mSegments.size();
	std::vector<bool> rescanned( count, false );
	// Identities of the unpaired Adds collected after the segment being considered.
	std::set<FileIdentity> later;
	for ( size_t s = count; s-- > 0; ) {
		for ( auto it = later.begin(); it != later.end(); ) {
			if ( halves[*it].paired() )
				it = later.erase( it );
			else
				++it;
		}
		// Copied, a rescan adds a segment.
		const Segment segment = mSegments[s];
		if ( segment.Owner && !later.empty() && segment.Owner->snapshotHasAny( later ) ) {
			size_t begin = mEvents.size();
			segment.Owner->watch();
			addHalves( halves, begin );
			addSegment( segment.Owner, begin );
			rescanned[s] = true;
		}
		for ( size_t i = segment.Begin; i < segment.End; i++ ) {
			const Event& event = mEvents[i];
			if ( event.ActionType != Actions::Add || event.Info.Inode == 0 )
				continue;
			if ( !halves[event.Info.identity()].paired() )
				later.insert( event.Info.identity() );
		}
	}
	return rescanned;
}

bool FileActionBatch::holdMoveSources( const HalvesMap& halves,
									   const std::vector<bool>& rescanned, const Moves& moves ) {
	// For each watch, the position of its first held event.
	std::unordered_map<Watcher*, size_t> holdFrom;
	// Whether a watch scanned before the segment was not scanned again since. When the
	// segment has a Delete, that watch may only see the destination on its next scan.
	bool canWait = false;
	for ( size_t s = 0; s < rescanned.size(); s++ ) {
		const Segment& segment = mSegments[s];
		for ( size_t i = segment.Begin; canWait && segment.Owner && i < segment.End; i++ ) {
			const Event& event = mEvents[i];
			if ( event.ActionType == Actions::Delete && event.Info.Inode != 0 &&
				 moves.MoveOf[i] == Moves::None &&
				 !halves.at( event.Info.identity() ).paired() ) {
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
	std::vector<Event> kept;
	kept.reserve( mEvents.size() );
	for ( size_t i = 0; i < mEvents.size(); i++ ) {
		size_t move = moves.MoveOf[i];
		if ( inHeldRange( move == Moves::None ? i : move ) )
			mHeld.push_back( std::move( mEvents[i] ) );
		else
			kept.push_back( std::move( mEvents[i] ) );
	}
	mEvents.swap( kept );
	return true;
}

void FileActionBatch::finishPass() {
	HalvesMap halves;
	addHalves( halves, 0 );
	bool unpaired = false;
	for ( const auto& identity : halves ) {
		if ( !identity.second.paired() ) {
			unpaired = true;
			break;
		}
	}
	std::vector<bool> rescanned;
	if ( unpaired )
		rescanned = rescanMoveSources( halves );

	Moves moves;
	bool hasMoves = matchMoves( moves );
	if ( unpaired && holdMoveSources( halves, rescanned, moves ) )
		hasMoves = matchMoves( moves );
	mSegments.clear();
	mReleased = 0;
	if ( hasMoves ) {
		followEnclosingMoves( moves );
		reconcileTrees( moves );
	}
	deliver( NULL, hasMoves ? &moves : NULL );
	pruneSourceTrees();
}

bool FileActionBatch::matchMoves( Moves& moves ) const {
	moves.MoveOf.assign( mEvents.size(), Moves::None );
	moves.SourceOf.assign( mEvents.size(), Moves::None );
	moves.Reconciled.clear();
	moves.Followers.clear();
	moves.Within.clear();
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

	struct Candidates {
		std::vector<size_t> Created;
		std::vector<size_t> Deleted;
	};

	std::unordered_map<FileIdentity, Candidates, FileIdentityHash> candidates;
	for ( size_t i = 0; i < mEvents.size(); i++ ) {
		const Event& event = mEvents[i];
		if ( event.Info.Inode == 0 ||
			 ( event.ActionType != Actions::Add && event.ActionType != Actions::Delete ) )
			continue;

		Candidates& identity = candidates[event.Info.identity()];
		if ( event.ActionType == Actions::Add )
			identity.Created.push_back( i );
		else
			identity.Deleted.push_back( i );
	}

	// The Add and Delete positions of each identity with one of each. Directories come first,
	// outermost first, by destination path: a moved directory claims the events inside it
	// before the moves nested in it are considered. Then the rest, in collection order.
	std::vector<std::pair<size_t, size_t>> pairs;
	for ( const auto& identity : candidates ) {
		const Candidates& entries = identity.second;
		if ( entries.Created.size() == 1 && entries.Deleted.size() == 1 )
			pairs.emplace_back( entries.Created[0], entries.Deleted[0] );
	}
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

	// The Deletes and Adds by path, built for the first directory move: the events inside a
	// directory are a range of each.
	std::vector<size_t> deletesByPath;
	std::vector<size_t> addsByPath;
	auto byPath = [&]( size_t a, size_t b ) {
		return mEvents[a].Info.Filepath < mEvents[b].Info.Filepath;
	};
	// The events inside a moved directory belong to its move, unless part of another move.
	// @param enclosing The move that claimed the directory's own event, if any: a directory
	// renamed or relocated inside a tree moved as a whole takes its contents back from it.
	auto claimContents = [&]( const std::vector<size_t>& byPathIndex, const std::string& directory,
							  size_t move, size_t enclosing ) {
		if ( directory.empty() )
			return;
		std::string prefix( directory );
		if ( prefix[prefix.size() - 1] != FileSystem::getOSSlash() )
			prefix += FileSystem::getOSSlash();
		auto it = std::lower_bound(
			byPathIndex.begin(), byPathIndex.end(), prefix,
			[&]( size_t i, const std::string& path ) { return mEvents[i].Info.Filepath < path; } );
		for ( ; it != byPathIndex.end(); ++it ) {
			const std::string& path = mEvents[*it].Info.Filepath;
			if ( path.compare( 0, prefix.size(), prefix ) != 0 )
				break;
			if ( path.size() > prefix.size() &&
				 ( moves.MoveOf[*it] == Moves::None ||
				   ( Moves::None != enclosing && moves.MoveOf[*it] == enclosing ) ) )
				moves.MoveOf[*it] = move;
		}
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

	// Whether a directory move's trees can be compared: the source left evidence of its tree,
	// and the destination watches inside the directory. Then its comparison reports the moves
	// nested in it.
	const std::unordered_map<std::string, FileIdentity> roots = evidenceRoots();
	const FileIdentity noIdentity( 0, 0 );
	auto comparesTrees = [&]( size_t deleted, size_t created ) {
		const Event& destination = mEvents[created];
		const Event& source = mEvents[deleted];
		return ( mSourceTrees.count( source.Info.identity() ) ||
				 enclosingRoot( roots, source.Info.Filepath, noIdentity ) != noIdentity ) &&
			   NULL != destination.Owner && !destination.Canceled &&
			   destination.Owner->collectTree( destination.Info.Filepath, NULL );
	};
	std::vector<bool> comparedTrees;

	bool hasMove = false;
	for ( const auto& pair : pairs ) {
		const size_t created = pair.first;
		const size_t deleted = pair.second;
		const size_t enclosingSource = moves.MoveOf[deleted];
		const size_t enclosingDestination = moves.MoveOf[created];
		if ( Moves::None != enclosingSource && enclosingSource == enclosingDestination &&
			 ( ( !comparedTrees.empty() && comparedTrees[enclosingSource] ) ||
			   keepsRelativePath( enclosingSource, deleted, created ) ) )
			continue;
		const Event& source = mEvents[deleted];
		const Event& destination = mEvents[created];
		if ( source.Directory == destination.Directory ||
			 !source.Info.sameInode( destination.Info ) ||
			 ( source.Info.isRegularFile() &&
			   ( source.Info.LinkCount != 1 || destination.Info.LinkCount != 1 ) ) ||
			 !metadataMatches( source.Info, destination.Info ) )
			continue;

		moves.MoveOf[deleted] = created;
		moves.MoveOf[created] = created;
		moves.SourceOf[created] = deleted;
		hasMove = true;
		if ( Moves::None != enclosingSource && enclosingSource == enclosingDestination )
			moves.Within[created] = enclosingSource;

		if ( source.Info.isDirectory() ) {
			if ( deletesByPath.empty() && addsByPath.empty() ) {
				for ( size_t i = 0; i < mEvents.size(); i++ ) {
					if ( mEvents[i].ActionType == Actions::Delete )
						deletesByPath.push_back( i );
					else if ( mEvents[i].ActionType == Actions::Add )
						addsByPath.push_back( i );
				}
				std::sort( deletesByPath.begin(), deletesByPath.end(), byPath );
				std::sort( addsByPath.begin(), addsByPath.end(), byPath );
			}
			claimContents( deletesByPath, source.Info.Filepath, created, enclosingSource );
			claimContents( addsByPath, destination.Info.Filepath, created, enclosingDestination );
			if ( !mSourceTrees.empty() && comparesTrees( deleted, created ) ) {
				if ( comparedTrees.empty() )
					comparedTrees.assign( mEvents.size(), false );
				comparedTrees[created] = true;
				moves.Reconciled.push_back( created );
			}
		}
	}
	return hasMove;
}

void FileActionBatch::followEnclosingMoves( Moves& moves ) {
	const size_t count = mEvents.size();
	// The events that may refer to a moved directory's new tree, by path. Deletes refer to
	// what was there before: they are not moved past the directory's arrival.
	std::vector<size_t> byPath;
	for ( size_t i = 0; i < count; i++ )
		if ( mEvents[i].ActionType != Actions::Delete && !mEvents[i].Info.Filepath.empty() )
			byPath.push_back( i );

	// For each event, the deepest directory move after it whose destination contains it. That
	// move follows the moves enclosing it the same way, so all of them come first.
	std::vector<size_t> after( count, Moves::None );
	bool sorted = false;
	bool reorders = false;
	for ( size_t move = 0; move < count; move++ ) {
		if ( moves.MoveOf[move] != move || !mEvents[move].Info.isDirectory() )
			continue;
		if ( !sorted ) {
			std::sort( byPath.begin(), byPath.end(), [&]( size_t a, size_t b ) {
				return mEvents[a].Info.Filepath < mEvents[b].Info.Filepath;
			} );
			sorted = true;
		}
		std::string prefix( mEvents[move].Info.Filepath );
		if ( prefix.back() != FileSystem::getOSSlash() )
			prefix += FileSystem::getOSSlash();
		auto it = std::lower_bound(
			byPath.begin(), byPath.end(), prefix,
			[&]( size_t i, const std::string& path ) { return mEvents[i].Info.Filepath < path; } );
		for ( ; it != byPath.end(); ++it ) {
			const std::string& path = mEvents[*it].Info.Filepath;
			if ( path.compare( 0, prefix.size(), prefix ) != 0 )
				break;
			if ( *it < move && path.size() > prefix.size() &&
				 ( Moves::None == after[*it] || mEvents[after[*it]].Info.Filepath.size() <
													mEvents[move].Info.Filepath.size() ) ) {
				after[*it] = move;
				reorders = true;
			}
		}
	}
	if ( !reorders )
		return;

	// Each event that follows a move goes right after it, in collection order, each followed by
	// its own followers. A move is always later than the events that follow it, so this ends.
	std::vector<std::vector<size_t>> followers( count );
	for ( size_t i = 0; i < count; i++ )
		if ( Moves::None != after[i] )
			followers[after[i]].push_back( i );
	std::vector<size_t> order;
	order.reserve( count );
	std::vector<size_t> stack;
	for ( size_t i = 0; i < count; i++ ) {
		if ( Moves::None != after[i] )
			continue;
		stack.push_back( i );
		while ( !stack.empty() ) {
			const size_t next = stack.back();
			stack.pop_back();
			order.push_back( next );
			const std::vector<size_t>& list = followers[next];
			for ( auto follower = list.rbegin(); follower != list.rend(); ++follower )
				stack.push_back( *follower );
		}
	}

	std::vector<size_t> position( count );
	for ( size_t i = 0; i < count; i++ )
		position[order[i]] = i;
	auto remap = [&]( const std::vector<size_t>& links ) {
		std::vector<size_t> remapped( count, Moves::None );
		for ( size_t i = 0; i < count; i++ )
			if ( Moves::None != links[i] )
				remapped[position[i]] = position[links[i]];
		return remapped;
	};
	moves.MoveOf = remap( moves.MoveOf );
	moves.SourceOf = remap( moves.SourceOf );
	for ( size_t& move : moves.Reconciled )
		move = position[move];
	std::unordered_map<size_t, size_t> within;
	for ( const auto& move : moves.Within )
		within[position[move.first]] = position[move.second];
	moves.Within.swap( within );
	std::vector<Event> events;
	events.reserve( count );
	for ( size_t i : order )
		events.push_back( std::move( mEvents[i] ) );
	mEvents.swap( events );
}

namespace {

/// An entry of a tree, by identity, pointing into a snapshot or an event.
struct TreeEntry {
	FileIdentity Identity;
	const FileInfo* Info;
};

/// One end of a directory move: the entries of the tree below the directory, by identity and by
/// path. Paths are compared below the root, without copying them.
struct TreeSide {
	size_t RootLength{ 0 };
	std::vector<TreeEntry> ById;
	std::vector<const FileInfo*> ByPath;

	/// @return The path below the root, starting with a separator.
	std::string relative( const FileInfo* entry ) const {
		return entry->Filepath.substr( RootLength );
	}

	void index() {
		std::sort( ById.begin(), ById.end(), []( const TreeEntry& a, const TreeEntry& b ) {
			return a.Identity < b.Identity ||
				   ( a.Identity == b.Identity && a.Info->Filepath < b.Info->Filepath );
		} );
		ById.erase( std::unique( ById.begin(), ById.end(),
								 []( const TreeEntry& a, const TreeEntry& b ) {
									 return a.Identity == b.Identity &&
											a.Info->Filepath == b.Info->Filepath;
								 } ),
					ById.end() );
	}

	/// Indexes the entries by path, once some differ.
	void indexPaths() {
		ByPath.clear();
		ByPath.reserve( ById.size() );
		for ( const TreeEntry& entry : ById )
			ByPath.push_back( entry.Info );
		const size_t length = RootLength;
		std::sort( ByPath.begin(), ByPath.end(), [length]( const FileInfo* a, const FileInfo* b ) {
			return a->Filepath.compare( length, std::string::npos, b->Filepath, length,
										std::string::npos ) < 0;
		} );
	}

	/// @return The entry at a path below the root, or NULL.
	const FileInfo* atPath( const std::string& relative ) const {
		const size_t length = RootLength;
		auto it = std::lower_bound( ByPath.begin(), ByPath.end(), relative,
									[length]( const FileInfo* entry, const std::string& path ) {
										return entry->Filepath.compare( length, std::string::npos,
																		path ) < 0;
									} );
		return it != ByPath.end() &&
					   0 == ( *it )->Filepath.compare( length, std::string::npos, relative )
				   ? *it
				   : NULL;
	}

	/// @return An entry with the identity, or NULL. Directories have one.
	const FileInfo* withIdentity( const FileIdentity& identity ) const {
		auto it = std::lower_bound(
			ById.begin(), ById.end(), identity,
			[]( const TreeEntry& entry, const FileIdentity& id ) { return entry.Identity < id; } );
		return it != ById.end() && it->Identity == identity ? it->Info : NULL;
	}
};

/// A name in a path, pointing into it.
struct NameRef {
	const char* Data;
	size_t Size;

	bool operator==( const NameRef& other ) const {
		return Size == other.Size && 0 == std::memcmp( Data, other.Data, Size );
	}
	bool operator!=( const NameRef& other ) const { return !( *this == other ); }
};

NameRef nameOf( const FileInfo* entry ) {
	const size_t slash = entry->Filepath.rfind( FileSystem::getOSSlash() );
	return { entry->Filepath.data() + slash + 1, entry->Filepath.size() - slash - 1 };
}

/// A name in a directory, by the directory's node.
struct Slot {
	int Parent;
	NameRef Name;

	bool operator==( const Slot& other ) const {
		return Parent == other.Parent && Name == other.Name;
	}
};

struct SlotHash {
	size_t operator()( const Slot& slot ) const {
		size_t hash = 14695981039346656037ull ^ static_cast<size_t>( slot.Parent );
		for ( size_t i = 0; i < slot.Name.Size; i++ )
			hash = ( hash ^ static_cast<unsigned char>( slot.Name.Data[i] ) ) * 1099511628211ull;
		return hash;
	}
};

} // namespace

/// Orders the changes that turn the tree before a directory move, as its Moved event leaves it
/// at the destination, into the tree the destination found, so that a listener can apply them
/// one after the other: each Delete, Add and Moved event names an existing entry, or a free name
/// in an existing directory. It works on the changed entries and their ancestors only: an entry
/// is changed unless its parent and name are the same at both ends, by identity, not by path.
/// Names point into the entries' paths, which outlive it.
class FileActionBatch::TreePlanner {
  public:
	TreePlanner( const TreeSide& before, const TreeSide& after, TreeChanges& output ) :
		mBefore( before ), mAfter( after ), mOutput( output ) {
		mNodes.emplace_back();
		mNodes[0].Present = true;
		mNodes[0].Directory = true;
	}

	/// Adds an entry that may have changed: at either end, or at both.
	void consider( const FileInfo* before, const FileInfo* after ) {
		const FileInfo* entry = before ? before : after;
		if ( entry->isDirectory() )
			directoryNode( entry->identity() );
		else
			fileNode( before, after );
	}

	/// @return False when the changes cannot be ordered: directories that wait for one another.
	bool plan();

  private:
	struct Node {
		const FileInfo* Before{ NULL };
		const FileInfo* After{ NULL };
		int BeforeParent{ -1 };
		int AfterParent{ -1 };
		/// In the tree being turned: its parent, whether its name is the one after, whether it
		/// is there, and how many changed entries it holds.
		int Parent{ -1 };
		int Children{ 0 };
		bool AtAfter{ false };
		bool Present{ false };
		bool Directory{ false };
		bool Changed{ false };
		/// Reported as deleted, to be created again: a move that waits in a cycle.
		bool Recreate{ false };
		bool Done{ false };
		bool Queued{ false };
	};

	const TreeSide& mBefore;
	const TreeSide& mAfter;
	TreeChanges& mOutput;
	std::vector<Node> mNodes;
	std::unordered_map<FileIdentity, int, FileIdentityHash> mDirectories;
	std::unordered_map<Slot, int, SlotHash> mOccupied;
	std::unordered_map<Slot, std::vector<int>, SlotHash> mSlotWaiters;
	std::unordered_map<int, std::vector<int>> mNodeWaiters;
	std::deque<int> mQueue;
	/// The changes not run yet.
	size_t mRemaining{ 0 };
	bool mFailed{ false };

	NameRef name( int index ) const {
		const Node& node = mNodes[index];
		return nameOf( node.AtAfter ? node.After : node.Before );
	}

	int parentNode( const TreeSide& side, const FileInfo* entry ) {
		const std::string relative = side.relative( entry );
		const size_t slash = relative.rfind( FileSystem::getOSSlash() );
		if ( 0 == slash )
			return 0;
		const FileInfo* parent = side.atPath( relative.substr( 0, slash ) );
		if ( NULL == parent || !parent->isDirectory() ) {
			mFailed = true;
			return 0;
		}
		return directoryNode( parent->identity() );
	}

	void resolve( int index ) {
		if ( mNodes[index].Before ) {
			const int parent = parentNode( mBefore, mNodes[index].Before );
			mNodes[index].BeforeParent = parent;
		}
		if ( mNodes[index].After ) {
			const int parent = parentNode( mAfter, mNodes[index].After );
			mNodes[index].AfterParent = parent;
		}
	}

	int directoryNode( const FileIdentity& identity ) {
		auto found = mDirectories.find( identity );
		if ( found != mDirectories.end() )
			return found->second;
		const int index = static_cast<int>( mNodes.size() );
		mDirectories[identity] = index;
		mNodes.emplace_back();
		mNodes[index].Directory = true;
		mNodes[index].Before = mBefore.withIdentity( identity );
		mNodes[index].After = mAfter.withIdentity( identity );
		resolve( index );
		return index;
	}

	void fileNode( const FileInfo* before, const FileInfo* after ) {
		const int index = static_cast<int>( mNodes.size() );
		mNodes.emplace_back();
		mNodes[index].Before = before;
		mNodes[index].After = after;
		resolve( index );
	}

	/// @return The node's path below the root, in the tree being turned.
	std::string path( int index ) const {
		std::vector<int> chain;
		for ( int node = index; node > 0; node = mNodes[node].Parent )
			chain.push_back( node );
		std::string result;
		for ( auto node = chain.rbegin(); node != chain.rend(); ++node ) {
			const NameRef part = name( *node );
			result += FileSystem::getOSSlash();
			result.append( part.Data, part.Size );
		}
		return result;
	}

	/// @return The absolute path to report, or nothing when the entry's own path below its
	/// root gives it.
	std::string absolute( const std::string& relative, const TreeSide& side,
						  const FileInfo* entry ) const {
		return 0 == entry->Filepath.compare( side.RootLength, std::string::npos, relative )
				   ? std::string()
				   : mOutput.DestinationRoot + relative;
	}

	void report( Action action, int index, const std::string& oldPath = std::string() ) {
		const Node& node = mNodes[index];
		const std::string now = path( index );
		if ( Actions::Delete == action ) {
			mOutput.Changes.push_back( { action, node.Before, NULL,
										 absolute( now, mBefore, node.Before ), "", Moves::None,
										 Moves::None } );
		} else if ( Actions::Moved == action ) {
			mOutput.Changes.push_back(
				{ action, node.After, node.Before, absolute( now, mAfter, node.After ),
				  absolute( oldPath, mBefore, node.Before ), Moves::None, Moves::None } );
		} else {
			mOutput.Changes.push_back( { action, node.After, NULL,
										 absolute( now, mAfter, node.After ), "", Moves::None,
										 Moves::None } );
		}
	}

	void enqueue( int index ) {
		if ( !mNodes[index].Queued && !mNodes[index].Done ) {
			mNodes[index].Queued = true;
			mQueue.push_back( index );
		}
	}

	void wake( std::vector<int>& waiters ) {
		std::vector<int> woken;
		woken.swap( waiters );
		for ( int index : woken )
			enqueue( index );
	}

	void wakeNode( int index ) {
		auto waiters = mNodeWaiters.find( index );
		if ( waiters != mNodeWaiters.end() )
			wake( waiters->second );
	}

	void place( int index, int parent, bool atAfter ) {
		Node& node = mNodes[index];
		node.Parent = parent;
		node.AtAfter = atAfter;
		node.Present = true;
		mOccupied[{ parent, name( index ) }] = index;
		mNodes[parent].Children++;
		wakeNode( parent );
		wakeNode( index );
	}

	void take( int index ) {
		const Slot slot{ mNodes[index].Parent, name( index ) };
		mOccupied.erase( slot );
		auto waiters = mSlotWaiters.find( slot );
		if ( waiters != mSlotWaiters.end() )
			wake( waiters->second );
		mNodes[slot.Parent].Children--;
		wakeNode( slot.Parent );
		mNodes[index].Present = false;
		wakeNode( index );
	}

	/// @return The entry below index on the way to node, when node is inside index's tree; else
	/// -1.
	int insideOf( int node, int index ) const {
		for ( int child = -1; node >= 0; child = node, node = mNodes[node].Parent )
			if ( node == index )
				return child < 0 ? index : child;
		return -1;
	}

	/// Runs the node's change when it can be applied now, or waits for what it needs.
	void run( int index );
};

void FileActionBatch::TreePlanner::run( int index ) {
	Node& node = mNodes[index];
	node.Queued = false;
	if ( node.Done )
		return;
	if ( node.Before && !node.After ) {
		// Entries that remain must move out first.
		if ( node.Children > 0 ) {
			mNodeWaiters[index].push_back( index );
			return;
		}
		report( Actions::Delete, index );
		take( index );
		mNodes[index].Done = true;
		mRemaining--;
		return;
	}
	const int parent = node.AfterParent;
	if ( !mNodes[parent].Present ) {
		mNodeWaiters[parent].push_back( index );
		return;
	}
	const Slot slot{ parent, nameOf( node.After ) };
	auto occupant = mOccupied.find( slot );
	if ( occupant != mOccupied.end() && occupant->second != index ) {
		mSlotWaiters[slot].push_back( index );
		return;
	}
	if ( node.Present && !node.Recreate ) {
		// A directory cannot move into its own tree: the entry that holds the destination must
		// move out first.
		const int holder = insideOf( parent, index );
		if ( holder >= 0 ) {
			mNodeWaiters[holder].push_back( index );
			return;
		}
		const std::string oldPath = path( index );
		take( index );
		place( index, parent, true );
		report( Actions::Moved, index, oldPath );
	} else {
		place( index, parent, true );
		report( Actions::Add, index );
	}
	mNodes[index].Done = true;
	mRemaining--;
}

bool FileActionBatch::TreePlanner::plan() {
	if ( mFailed )
		return false;
	// The tree before, with the changed entries indexed by their place.
	std::vector<int> deletions, arrivals;
	for ( size_t i = 1; i < mNodes.size(); i++ ) {
		Node& node = mNodes[i];
		node.Changed = !node.Before || !node.After || node.BeforeParent != node.AfterParent ||
					   nameOf( node.Before ) != nameOf( node.After );
		if ( node.Before ) {
			node.Present = true;
			node.Parent = node.BeforeParent;
		}
	}
	for ( size_t i = 1; i < mNodes.size(); i++ ) {
		Node& node = mNodes[i];
		if ( !node.Changed ) {
			node.Done = true;
			continue;
		}
		if ( node.Present ) {
			mOccupied[{ node.Parent, name( static_cast<int>( i ) ) }] = static_cast<int>( i );
			mNodes[node.Parent].Children++;
		}
		( node.After ? arrivals : deletions ).push_back( static_cast<int>( i ) );
	}
	// Deletions first, each before its parent, then arrivals, parents first: the natural order,
	// which the dependencies change where they must.
	auto pathOrder = [this]( const TreeSide& side, const FileInfo* a, const FileInfo* b ) {
		return a->Filepath.compare( side.RootLength, std::string::npos, b->Filepath,
									side.RootLength, std::string::npos ) < 0;
	};
	std::sort( deletions.begin(), deletions.end(), [&]( int a, int b ) {
		return pathOrder( mBefore, mNodes[b].Before, mNodes[a].Before );
	} );
	std::sort( arrivals.begin(), arrivals.end(), [&]( int a, int b ) {
		return pathOrder( mAfter, mNodes[a].After, mNodes[b].After );
	} );
	for ( int index : deletions )
		enqueue( index );
	for ( int index : arrivals )
		enqueue( index );
	mRemaining = deletions.size() + arrivals.size();
	// The next arrival that may be recreated to break a cycle: one stops being a candidate once
	// done or recreated, and none becomes one, so each is looked at once.
	size_t candidate = 0;

	for ( ;; ) {
		while ( !mQueue.empty() ) {
			const int index = mQueue.front();
			mQueue.pop_front();
			run( index );
		}
		// Whatever is left waits in a cycle, as files swapped do. A file is reported as deleted
		// and created again, which breaks it. Directories that wait for one another are not.
		if ( 0 == mRemaining )
			return true;
		while ( candidate < arrivals.size() ) {
			const Node& node = mNodes[arrivals[candidate]];
			if ( !node.Done && !node.Directory && node.Present && !node.Recreate )
				break;
			candidate++;
		}
		if ( candidate == arrivals.size() )
			return false;
		const int recreated = arrivals[candidate];
		report( Actions::Delete, recreated );
		take( recreated );
		mNodes[recreated].Recreate = true;
		enqueue( recreated );
	}
}

void FileActionBatch::reconcileTrees( Moves& moves ) {
	if ( moves.Reconciled.empty() )
		return;
	const std::unordered_map<std::string, FileIdentity> roots = evidenceRoots();
	// The evidence of a tree by path, built once for the directories that left it.
	std::unordered_map<FileIdentity, std::vector<const FileInfo*>, FileIdentityHash> indexes;
	auto indexedEvidence =
		[&indexes]( const std::unordered_map<FileIdentity, std::vector<FileInfoList>,
											 FileIdentityHash>::const_iterator& tree )
		-> const std::vector<const FileInfo*>& {
		auto found = indexes.find( tree->first );
		if ( found != indexes.end() )
			return found->second;
		std::vector<const FileInfo*>& index = indexes[tree->first];
		for ( const FileInfoList& list : tree->second )
			for ( const FileInfo& entry : list )
				index.push_back( &entry );
		std::sort( index.begin(), index.end(), []( const FileInfo* a, const FileInfo* b ) {
			return a->Filepath < b->Filepath;
		} );
		return index;
	};
	// The entries that are halves of moves of their own, between the tree and the rest: those
	// moves report them.
	std::unordered_set<FileIdentity, FileIdentityHash> movedOnTheirOwn;
	for ( size_t i = 0; i < mEvents.size(); i++ )
		if ( moves.MoveOf[i] == i )
			movedOnTheirOwn.insert( mEvents[i].Info.identity() );
	// The moves of their own that leave a compared tree, and those that enter one, by the tree's
	// move. An entry that left is deleted from its tree, unless its move was reported while the
	// directory was still at its source; what moved with it leaves the tree, and what moved with
	// one that entered is reported by its move.
	std::unordered_map<std::string, size_t> sourceRoots, destinationRoots;
	for ( size_t move : moves.Reconciled ) {
		const std::string& source = mEvents[moves.SourceOf[move]].Info.Filepath;
		const std::string& destination = mEvents[move].Info.Filepath;
		sourceRoots[source.substr( 0, directoryLength( source ) )] = move;
		destinationRoots[destination.substr( 0, directoryLength( destination ) )] = move;
	}
	// The tree whose root contains the path, if any.
	auto treeOf = []( const std::unordered_map<std::string, size_t>& roots,
					  const std::string& path ) {
		return enclosingRoot( roots, path, Moves::None );
	};
	// The held Deletes inside a compared tree's source, and the held Adds inside its destination:
	// halves of moves held to the next pass. The comparison counts them as deleted and created,
	// and supersedes them.
	std::unordered_map<size_t, std::vector<size_t>> heldDeletes, heldAdds;
	for ( size_t h = 0; h < mHeld.size(); h++ ) {
		const Event& held = mHeld[h];
		if ( held.Info.Inode == 0 )
			continue;
		if ( Actions::Delete == held.ActionType ) {
			const size_t tree = treeOf( sourceRoots, held.Info.Filepath );
			if ( Moves::None != tree )
				heldDeletes[tree].push_back( h );
		} else if ( Actions::Add == held.ActionType && !held.Canceled ) {
			const size_t tree = treeOf( destinationRoots, held.Info.Filepath );
			if ( Moves::None != tree )
				heldAdds[tree].push_back( h );
		}
	}
	// The directories added without a Delete to pair them, as kqueue reports a directory that
	// arrived from a tree whose entries it did not report: when one left a compared tree, it is
	// delivered with what it holds.
	std::vector<Event*> unpairedDirectories;
	for ( size_t i = 0; i < mEvents.size(); i++ )
		if ( Actions::Add == mEvents[i].ActionType && Moves::None == moves.MoveOf[i] &&
			 mEvents[i].Info.Inode != 0 && mEvents[i].Info.isDirectory() )
			unpairedDirectories.push_back( &mEvents[i] );
	// Held to the next pass, where it may still pair.
	for ( Event& held : mHeld )
		if ( Actions::Add == held.ActionType && !held.Canceled && held.Info.Inode != 0 &&
			 held.Info.isDirectory() )
			unpairedDirectories.push_back( &held );
	std::unordered_map<size_t, std::vector<size_t>> leaving, entering;
	for ( size_t i = 0; i < mEvents.size(); i++ ) {
		if ( moves.MoveOf[i] != i )
			continue;
		const size_t left = treeOf( sourceRoots, mEvents[moves.SourceOf[i]].Info.Filepath );
		if ( Moves::None != left && left != i )
			leaving[left].push_back( i );
		const size_t entered = treeOf( destinationRoots, mEvents[i].Info.Filepath );
		if ( Moves::None != entered && entered != i )
			entering[entered].push_back( i );
	}
	// The Deletes each compared move claimed, found once.
	std::unordered_map<size_t, std::vector<size_t>> claimed;
	{
		std::vector<bool> compared( mEvents.size(), false );
		for ( size_t move : moves.Reconciled )
			compared[move] = true;
		for ( size_t i = 0; i < mEvents.size(); i++ ) {
			const size_t move = moves.MoveOf[i];
			if ( mEvents[i].ActionType == Actions::Delete && Moves::None != move &&
				 compared[move] && i != moves.SourceOf[move] )
				claimed[move].push_back( i );
		}
	}

	for ( size_t move : moves.Reconciled ) {
		const Event& destination = mEvents[move];
		const Event& source = mEvents[moves.SourceOf[move]];
		// The evidence of the directory's own tree, or of the tree it left, of which the part
		// below it is the evidence of its own.
		auto tree = mSourceTrees.find( source.Info.identity() );
		const bool ownTree = tree != mSourceTrees.end();
		if ( !ownTree ) {
			const FileIdentity enclosing =
				enclosingRoot( roots, source.Info.Filepath, FileIdentity( 0, 0 ) );
			tree = mSourceTrees.find( enclosing );
		}
		std::vector<const FileInfo*> destinationTree;
		if ( tree == mSourceTrees.end() || NULL == destination.Owner ||
			 !destination.Owner->collectTree( destination.Info.Filepath, &destinationTree ) )
			continue;

		const std::string& sourceRoot = source.Info.Filepath;
		const std::string& destinationRoot = destination.Info.Filepath;
		TreeSide before, after;
		before.RootLength = directoryLength( sourceRoot );
		after.RootLength = directoryLength( destinationRoot );
		auto inside = []( const FileInfo& entry, const std::string& root, size_t length ) {
			return entry.Inode != 0 && entry.Filepath.size() > length + 1 &&
				   entry.Filepath[length] == FileSystem::getOSSlash() &&
				   entry.Filepath.compare( 0, length, root, 0, length ) == 0;
		};
		// The entries that left the tree, by identity, with their moves; the directories among
		// them, and those that entered it, whose trees moved with them.
		std::unordered_map<FileIdentity, size_t, FileIdentityHash> leftTree;
		std::unordered_set<std::string> carriedOut, carriedIn;
		auto movedTrees = leaving.find( move );
		if ( movedTrees != leaving.end() ) {
			for ( size_t own : movedTrees->second ) {
				const FileInfo& entry = mEvents[moves.SourceOf[own]].Info;
				leftTree[entry.identity()] = own;
				if ( entry.isDirectory() )
					carriedOut.insert(
						entry.Filepath.substr( 0, directoryLength( entry.Filepath ) ) );
			}
		}
		movedTrees = entering.find( move );
		if ( movedTrees != entering.end() )
			for ( size_t own : movedTrees->second )
				if ( mEvents[own].Info.isDirectory() )
					carriedIn.insert( mEvents[own].Info.Filepath.substr(
						0, directoryLength( mEvents[own].Info.Filepath ) ) );
		// Whether the entry moved with a directory that left or entered the tree.
		auto carried = []( const std::unordered_set<std::string>& directories,
						   const std::string& path ) {
			if ( directories.empty() )
				return false;
			for ( size_t slash = path.rfind( FileSystem::getOSSlash() );
				  slash != std::string::npos && slash > 0;
				  slash = path.rfind( FileSystem::getOSSlash(), slash - 1 ) )
				if ( directories.count( path.substr( 0, slash ) ) )
					return true;
			return false;
		};
		auto addBefore = [&]( const FileInfo& entry ) {
			if ( inside( entry, sourceRoot, before.RootLength ) &&
				 ( !movedOnTheirOwn.count( entry.identity() ) ||
				   leftTree.count( entry.identity() ) ) &&
				 !carried( carriedOut, entry.Filepath ) )
				before.ById.push_back( { entry.identity(), &entry } );
		};
		// The snapshots given up, and the Deletes the move claimed: a generic watch reports one
		// for each entry of a directory it finds gone, which empties its snapshot, possibly
		// before its tree was given up.
		if ( ownTree ) {
			for ( const FileInfoList& list : tree->second )
				for ( const FileInfo& entry : list )
					addBefore( entry );
		} else {
			const std::vector<const FileInfo*>& index = indexedEvidence( tree );
			const std::string below =
				sourceRoot.substr( 0, before.RootLength ) + FileSystem::getOSSlash();
			for ( auto entry =
					  std::lower_bound( index.begin(), index.end(), below,
										[]( const FileInfo* info, const std::string& path ) {
											return info->Filepath < path;
										} );
				  entry != index.end() &&
				  0 == ( *entry )->Filepath.compare( 0, below.size(), below );
				  ++entry )
				addBefore( **entry );
		}
		auto deletes = claimed.find( move );
		if ( deletes != claimed.end() )
			for ( size_t i : deletes->second )
				addBefore( mEvents[i].Info );
		// A generic watch reported the Delete of an entry that left: the move of its own paired
		// it, so the tree's move did not claim it.
		for ( const auto& left : leftTree )
			addBefore( mEvents[moves.SourceOf[left.second]].Info );
		std::unordered_map<FileIdentity, size_t, FileIdentityHash> superseded;
		auto held = heldDeletes.find( move );
		if ( held != heldDeletes.end() ) {
			for ( size_t h : held->second ) {
				addBefore( mHeld[h].Info );
				superseded[mHeld[h].Info.identity()] = h;
			}
		}
		held = heldAdds.find( move );
		if ( held != heldAdds.end() )
			for ( size_t h : held->second )
				superseded[mHeld[h].Info.identity()] = h;
		after.ById.reserve( destinationTree.size() );
		for ( const FileInfo* entry : destinationTree )
			if ( inside( *entry, destinationRoot, after.RootLength ) &&
				 !movedOnTheirOwn.count( entry->identity() ) &&
				 !carried( carriedIn, entry->Filepath ) )
				after.ById.push_back( { entry->identity(), entry } );
		before.index();
		after.index();
		for ( Event* added : unpairedDirectories ) {
			const FileIdentity identity = added->Info.identity();
			if ( before.withIdentity( identity ) && !after.withIdentity( identity ) )
				added->WithContents = true;
		}

		// The entries whose path below the root differs, by identity. Hard links share one:
		// the links whose paths match are kept, and the rest are deleted and created.
		auto samePlace = [&]( const FileInfo* a, const FileInfo* b ) {
			return a->Filepath.compare( before.RootLength, std::string::npos, b->Filepath,
										after.RootLength, std::string::npos ) == 0;
		};
		std::vector<std::pair<const FileInfo*, const FileInfo*>> differing;
		const FileInfo* const none = NULL;
		for ( size_t b = 0, a = 0; b < before.ById.size() || a < after.ById.size(); ) {
			const FileIdentity identity =
				b < before.ById.size() && ( a == after.ById.size() ||
											before.ById[b].Identity < after.ById[a].Identity )
					? before.ById[b].Identity
					: after.ById[a].Identity;
			size_t bEnd = b, aEnd = a;
			while ( bEnd < before.ById.size() && before.ById[bEnd].Identity == identity )
				bEnd++;
			while ( aEnd < after.ById.size() && after.ById[aEnd].Identity == identity )
				aEnd++;
			if ( bEnd - b <= 1 && aEnd - a <= 1 ) {
				// The common case, without allocating.
				if ( bEnd == b )
					differing.emplace_back( none, after.ById[a].Info );
				else if ( aEnd == a )
					differing.emplace_back( before.ById[b].Info, none );
				else if ( !samePlace( before.ById[b].Info, after.ById[a].Info ) )
					differing.emplace_back( before.ById[b].Info, after.ById[a].Info );
			} else {
				std::vector<const FileInfo*> gone, come;
				for ( size_t i = b; i < bEnd; i++ )
					gone.push_back( before.ById[i].Info );
				for ( size_t i = a; i < aEnd; i++ )
					come.push_back( after.ById[i].Info );
				for ( auto g = gone.begin(); g != gone.end(); ) {
					auto c = std::find_if( come.begin(), come.end(), [&]( const FileInfo* entry ) {
						return samePlace( *g, entry );
					} );
					if ( c != come.end() ) {
						come.erase( c );
						g = gone.erase( g );
					} else {
						++g;
					}
				}
				if ( gone.size() == 1 && come.size() == 1 ) {
					differing.emplace_back( gone[0], come[0] );
				} else {
					for ( const FileInfo* entry : gone )
						differing.emplace_back( entry, none );
					for ( const FileInfo* entry : come )
						differing.emplace_back( none, entry );
				}
			}
			b = bEnd;
			a = aEnd;
		}
		if ( differing.empty() )
			continue;
		before.indexPaths();
		after.indexPaths();

		TreeChanges& changes = moves.Followers[move];
		changes.DestinationRoot = destinationRoot.substr( 0, after.RootLength );
		changes.SourceLength = before.RootLength;
		TreePlanner planner( before, after, changes );
		for ( const auto& entry : differing )
			planner.consider( entry.first, entry.second );
		// A directory replaced by another at the same path: the entries it held moved out of it,
		// even those whose path is the same.
		std::set<std::string> replaced;
		for ( const auto& entry : differing ) {
			if ( entry.first && entry.first->isDirectory() ) {
				const std::string place = before.relative( entry.first );
				const FileInfo* now = after.atPath( place );
				if ( now && now->isDirectory() && now->identity() != entry.first->identity() )
					replaced.insert( place );
			}
		}
		for ( const std::string& place : replaced ) {
			const std::string prefix = place + FileSystem::getOSSlash();
			const size_t length = before.RootLength;
			auto it = std::lower_bound( before.ByPath.begin(), before.ByPath.end(), prefix,
										[length]( const FileInfo* entry, const std::string& path ) {
											return entry->Filepath.compare(
													   length, std::string::npos, path ) < 0;
										} );
			for ( ; it != before.ByPath.end(); ++it ) {
				const std::string relative = before.relative( *it );
				if ( relative.compare( 0, prefix.size(), prefix ) != 0 )
					break;
				if ( relative.find( FileSystem::getOSSlash(), prefix.size() ) != std::string::npos )
					continue;
				const FileInfo* now = after.atPath( relative );
				if ( now && now->identity() == ( *it )->identity() )
					planner.consider( *it, now );
			}
		}

		if ( !planner.plan() ) {
			// Directories that wait for one another, which events cannot order: the tree is
			// reported replaced, every entry deleted, each before its parent, then created.
			changes.Changes.clear();
			for ( auto entry = before.ByPath.rbegin(); entry != before.ByPath.rend(); ++entry )
				changes.Changes.push_back(
					{ Actions::Delete, *entry, NULL, "", "", Moves::None, Moves::None } );
			for ( const FileInfo* entry : after.ByPath )
				changes.Changes.push_back(
					{ Actions::Add, entry, NULL, "", "", Moves::None, Moves::None } );
		}
		for ( TreeChange& change : changes.Changes ) {
			if ( Actions::Moved == change.ActionType )
				continue;
			auto replaced = superseded.find( change.Entry->identity() );
			if ( replaced != superseded.end() &&
				 ( Actions::Delete == change.ActionType ) ==
					 ( Actions::Delete == mHeld[replaced->second].ActionType ) )
				change.Held = replaced->second;
			if ( Actions::Delete != change.ActionType || leftTree.empty() )
				continue;
			auto left = leftTree.find( change.Entry->identity() );
			if ( left != leftTree.end() )
				change.Leaving = left->second;
		}
		if ( changes.Changes.empty() )
			moves.Followers.erase( move );
	}
}

void FileActionBatch::deliverEvent( FileWatchListener* listener, size_t index ) {
	const Event& event = mEvents[index];
	if ( event.Canceled || event.Superseded )
		return;
	FileWatchListener* target = event.Listener ? event.Listener : listener;
	if ( target )
		target->handleFileAction( event.Watch, event.Directory, event.Filename, event.ActionType,
								  event.OldFilename );
}

void FileActionBatch::deliver( FileWatchListener* listener, const Moves* moves ) {
	// Indexed, and the flags read at each step: a listener may cancel events of a removed watch.
	// The directories delivered as Adds of what they hold: what moves into them afterwards was
	// reported with them.
	std::unordered_set<std::string> addedTrees;
	// The source directories of the trees' changes delivered as Deletes: what moves out of them
	// afterwards left with them.
	std::unordered_set<std::string> deletedTrees;
	// Whether the path is below one of the directories, each given with a final separator: its
	// own prefixes are looked up, as many as its depth.
	auto inside = []( const std::unordered_set<std::string>& trees, const std::string& path ) {
		if ( trees.empty() )
			return false;
		for ( size_t slash = path.find( FileSystem::getOSSlash() ); slash != std::string::npos;
			  slash = path.find( FileSystem::getOSSlash(), slash + 1 ) )
			if ( trees.count( path.substr( 0, slash + 1 ) ) )
				return true;
		return false;
	};

	// Reports what a directory delivered as an Add holds, which a Moved event would have said:
	// what moves into it afterwards was reported with it.
	auto addContents = [&]( FileWatchListener* target, size_t i ) {
		const Event& directory = mEvents[i];
		std::vector<const FileInfo*> contents;
		if ( !directory.Info.isDirectory() || NULL == directory.Owner ||
			 !directory.Owner->collectTree( directory.Info.Filepath, &contents ) )
			return;
		addedTrees.insert(
			directory.Info.Filepath.substr( 0, directoryLength( directory.Info.Filepath ) ) +
			FileSystem::getOSSlash() );
		std::sort( contents.begin(), contents.end(), []( const FileInfo* a, const FileInfo* b ) {
			return a->Filepath < b->Filepath;
		} );
		for ( const FileInfo* entry : contents ) {
			if ( directory.Canceled )
				break;
			target->handleFileAction(
				directory.Watch, FileSystem::pathRemoveFileName( entry->Filepath ),
				FileSystem::fileNameFromPath( entry->Filepath ), Actions::Add, "" );
		}
	};

	// An event that is not a half of a move.
	auto deliverOrdinary = [&]( size_t i ) {
		const Event& event = mEvents[i];
		if ( Actions::Add == event.ActionType && inside( addedTrees, event.Info.Filepath ) )
			return;
		deliverEvent( listener, i );
		FileWatchListener* target = event.Listener ? event.Listener : listener;
		if ( target && !event.Canceled && event.WithContents )
			addContents( target, i );
	};

	if ( !moves ) {
		for ( size_t i = 0; i < mEvents.size(); i++ )
			deliverOrdinary( i );
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
	enum class MoveState : unsigned char { Unresolved, Delivered, Broken };
	// By the Add position of each move.
	std::vector<MoveState> state( mEvents.size(), MoveState::Unresolved );
	// The moves of their own that left a moved tree after its Moved event: the tree's changes
	// reported the entry deleted, so they are reported as Adds, and their sources not at all.
	std::vector<bool> converted( mEvents.size(), false );
	for ( size_t i = 0; i < mEvents.size(); i++ )
		if ( moves->MoveOf[i] == i && mEvents[moves->SourceOf[i]].Superseded )
			converted[i] = true;
	std::unordered_map<Watcher*, std::deque<size_t>> waiting;

	// The moves out of a deleted directory, by its Delete: it waits for them, or a listener
	// would no longer find their sources. A directory's Delete comes before the Deletes inside it.
	std::unordered_map<size_t, std::vector<size_t>> movesOut;
	bool deletesDirectories = false;
	for ( size_t i = 0; i < mEvents.size() && !deletesDirectories; i++ )
		deletesDirectories = Actions::Delete == mEvents[i].ActionType &&
							 Moves::None == moves->MoveOf[i] && mEvents[i].Info.isDirectory();
	if ( deletesDirectories ) {
		std::vector<std::pair<const std::string*, size_t>> sources;
		for ( size_t i = 0; i < mEvents.size(); i++ )
			if ( moves->MoveOf[i] == i )
				sources.emplace_back( &mEvents[moves->SourceOf[i]].Info.Filepath, i );
		if ( !sources.empty() ) {
			std::sort( sources.begin(), sources.end(),
					   []( const std::pair<const std::string*, size_t>& a,
						   const std::pair<const std::string*, size_t>& b ) {
						   return *a.first < *b.first;
					   } );
			for ( size_t i = 0; i < mEvents.size(); i++ ) {
				const Event& event = mEvents[i];
				if ( Actions::Delete != event.ActionType || Moves::None != moves->MoveOf[i] ||
					 !event.Info.isDirectory() )
					continue;
				const std::string below =
					event.Info.Filepath.substr( 0, directoryLength( event.Info.Filepath ) ) +
					FileSystem::getOSSlash();
				for ( auto source = std::lower_bound(
						  sources.begin(), sources.end(), below,
						  []( const std::pair<const std::string*, size_t>& entry,
							  const std::string& path ) { return *entry.first < path; } );
					  source != sources.end() &&
					  0 == source->first->compare( 0, below.size(), below );
					  ++source )
					movesOut[i].push_back( source->second );
			}
		}
	}

	// Whether a source event of a move waits for its destination. Not when both belong to one
	// registration, which is removed with both, or when the move is resolved. A deleted
	// directory waits for the moves out of it.
	auto waitsForMove = [&]( size_t i ) {
		const size_t move = moves->MoveOf[i];
		if ( Moves::None == move ) {
			auto out = movesOut.find( i );
			if ( out == movesOut.end() )
				return false;
			for ( size_t pending : out->second )
				if ( MoveState::Unresolved == state[pending] )
					return true;
			return false;
		}
		return move != i && mEvents[move].Owner != mEvents[i].Owner &&
			   MoveState::Unresolved == state[move];
	};

	auto process = [&]( size_t i ) {
		const size_t move = moves->MoveOf[i];
		if ( Moves::None == move ) {
			deliverOrdinary( i );
		} else if ( move != i ) {
			if ( MoveState::Broken == state[move] && !converted[move] )
				deliverEvent( listener, i );
		} else if ( mEvents[i].Canceled || mEvents[i].Superseded ) {
			// A move whose Add a moved tree's changes already reported: what remains of it is
			// its source's Delete.
			state[i] = MoveState::Broken;
		} else {
			state[i] = MoveState::Delivered;
			const Event& destination = mEvents[i];
			FileWatchListener* target = destination.Listener ? destination.Listener : listener;
			if ( inside( addedTrees, destination.Info.Filepath ) ) {
				// Reported with that directory: what remains is its source's Delete, unless the
				// source was already reported deleted.
				const size_t source = moves->SourceOf[i];
				if ( !converted[i] && !inside( deletedTrees, mEvents[source].Info.Filepath ) ) {
					deliverEvent( listener, source );
					mEvents[source].Superseded = true;
				}
				return;
			}
			if ( inside( deletedTrees, mEvents[moves->SourceOf[i]].Info.Filepath ) )
				converted[i] = true;
			// A move inside a directory move, from where that move's Moved event put it.
			std::string oldPath;
			auto enclosing = moves->Within.find( i );
			if ( enclosing != moves->Within.end() ) {
				const size_t outer = enclosing->second;
				if ( MoveState::Delivered == state[outer] && !converted[outer] ) {
					const std::string& from = mEvents[moves->SourceOf[outer]].Info.Filepath;
					oldPath =
						mEvents[outer].Info.Filepath +
						mEvents[moves->SourceOf[i]].Info.Filepath.substr( directoryLength( from ) );
				}
			}
			if ( target && converted[i] ) {
				target->handleFileAction( destination.Watch, destination.Directory,
										  destination.Filename, Actions::Add, "" );
				addContents( target, i );
			} else if ( target ) {
				const Event& source = mEvents[moves->SourceOf[i]];
				target->handleFileAction(
					destination.Watch, destination.Directory, destination.Filename, Actions::Moved,
					oldPath.empty() ? FileSystem::canonicalSourcePath( source.Info.Filepath )
									: oldPath );
				// What changed inside a moved directory, unless a listener removed its
				// registration meanwhile.
				auto followers = moves->Followers.find( i );
				if ( followers != moves->Followers.end() ) {
					const TreeChanges& changes = followers->second;
					for ( const TreeChange& change : changes.Changes ) {
						if ( mEvents[i].Canceled )
							break;
						if ( Moves::None != change.Leaving ) {
							if ( MoveState::Unresolved != state[change.Leaving] )
								continue;
							converted[change.Leaving] = true;
						}
						if ( Moves::None != change.Held )
							mHeld[change.Held].Superseded = true;
						if ( Actions::Delete == change.ActionType && change.Entry->isDirectory() )
							deletedTrees.insert(
								change.Entry->Filepath.substr(
									0, directoryLength( change.Entry->Filepath ) ) +
								FileSystem::getOSSlash() );
						// Where the directory's Moved event put an entry of the source.
						auto rebased = [&changes]( const FileInfo* entry ) {
							return changes.DestinationRoot +
								   entry->Filepath.substr( changes.SourceLength );
						};
						const std::string path = !change.Path.empty() ? change.Path
												 : Actions::Delete == change.ActionType
													 ? rebased( change.Entry )
													 : change.Entry->Filepath;
						const std::string oldPath = !change.OldPath.empty() || NULL == change.Source
														? change.OldPath
														: rebased( change.Source );
						target->handleFileAction(
							destination.Watch, FileSystem::pathRemoveFileName( path ),
							FileSystem::fileNameFromPath( path ), change.ActionType, oldPath );
					}
				}
			}
		}
	};

	// Delivers the waiting events that no longer wait, until none does.
	auto release = [&]() {
		for ( bool progress = true; progress; ) {
			progress = false;
			for ( auto it = waiting.begin(); it != waiting.end(); ) {
				std::deque<size_t>& queue = it->second;
				while ( !queue.empty() && !waitsForMove( queue.front() ) ) {
					const size_t i = queue.front();
					queue.pop_front();
					process( i );
					progress = true;
				}
				it = queue.empty() ? waiting.erase( it ) : std::next( it );
			}
		}
	};

	for ( size_t i = 0; i < mEvents.size(); i++ ) {
		auto queue = waiting.find( mEvents[i].Owner );
		if ( queue != waiting.end() || waitsForMove( i ) ) {
			waiting[mEvents[i].Owner].push_back( i );
			continue;
		}
		process( i );
		if ( moves->MoveOf[i] == i && !waiting.empty() )
			release();
	}

	// Moves whose registrations wait on each other, as crossed moves between two registrations
	// do, are never resolved: they are delivered as ordinary events, in collection order, so
	// that no source event is skipped for a destination that a later callback may still remove.
	// The events of such a destination's registration inside a moved directory, which the loop
	// above skipped, follow the directory's Add, which they come after.
	if ( !waiting.empty() ) {
		std::vector<size_t> rest;
		for ( const auto& queue : waiting )
			rest.insert( rest.end(), queue.second.begin(), queue.second.end() );
		std::sort( rest.begin(), rest.end() );
		std::vector<bool> inRest( mEvents.size(), false );
		std::vector<bool> unresolved( mEvents.size(), false );
		for ( size_t i : rest ) {
			inRest[i] = true;
			const size_t move = moves->MoveOf[i];
			if ( Moves::None != move && move != i && MoveState::Unresolved == state[move] )
				unresolved[move] = true;
		}
		// The skipped events of each unresolved destination's registration, in order.
		std::unordered_map<size_t, std::vector<size_t>> skipped;
		for ( size_t j = 0; j < mEvents.size(); j++ ) {
			const size_t move = moves->MoveOf[j];
			if ( Moves::None != move && move != j && unresolved[move] && !inRest[j] &&
				 mEvents[j].Owner == mEvents[move].Owner )
				skipped[move].push_back( j );
		}
		for ( size_t i : rest ) {
			const size_t move = moves->MoveOf[i];
			if ( Moves::None == move || !unresolved[move] ) {
				process( i );
				continue;
			}
			if ( move != i ) {
				if ( !converted[move] )
					deliverEvent( listener, i );
				continue;
			}
			deliverEvent( listener, i );
			auto members = skipped.find( i );
			if ( members != skipped.end() )
				for ( size_t j : members->second )
					deliverEvent( listener, j );
		}
	}
	clear();
}

void FileActionBatch::dispatch( FileWatchListener* listener, bool detectMoves ) {
	Moves moves;
	bool hasMoves = detectMoves && matchMoves( moves );
	if ( hasMoves )
		followEnclosingMoves( moves );
	deliver( listener, hasMoves ? &moves : NULL );
}

} // namespace efsw
