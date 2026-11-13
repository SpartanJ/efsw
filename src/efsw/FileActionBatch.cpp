#include <efsw/FileActionBatch.hpp>
#include <efsw/FileSystem.hpp>
#include <algorithm>
#include <deque>
#include <iterator>
#include <set>
#include <unordered_map>

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
	mHeld.erase( std::remove_if( mHeld.begin(), mHeld.end(),
								 [watcher]( const Event& event ) { return event.Owner == watcher; } ),
				 mHeld.end() );
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
	if ( hasMoves )
		followEnclosingMoves( moves );
	deliver( NULL, hasMoves ? &moves : NULL );
}

bool FileActionBatch::matchMoves( Moves& moves ) const {
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

	bool hasMove = false;
	for ( const auto& pair : pairs ) {
		const size_t created = pair.first;
		const size_t deleted = pair.second;
		const size_t enclosingSource = moves.MoveOf[deleted];
		const size_t enclosingDestination = moves.MoveOf[created];
		if ( Moves::None != enclosingSource && enclosingSource == enclosingDestination &&
			 keepsRelativePath( enclosingSource, deleted, created ) )
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
	std::vector<Event> events;
	events.reserve( count );
	for ( size_t i : order )
		events.push_back( std::move( mEvents[i] ) );
	mEvents.swap( events );
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
	enum class MoveState : unsigned char { Unresolved, Delivered, Broken };
	// By the Add position of each move.
	std::vector<MoveState> state( mEvents.size(), MoveState::Unresolved );
	std::unordered_map<Watcher*, std::deque<size_t>> waiting;

	// Whether a source event of a move waits for its destination. Not when both belong to one
	// registration, which is removed with both, or when the move is resolved.
	auto waitsForMove = [&]( size_t i ) {
		const size_t move = moves->MoveOf[i];
		return Moves::None != move && move != i && mEvents[move].Owner != mEvents[i].Owner &&
			   MoveState::Unresolved == state[move];
	};

	auto process = [&]( size_t i ) {
		const size_t move = moves->MoveOf[i];
		if ( Moves::None == move ) {
			deliverEvent( listener, i );
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
				target->handleFileAction( destination.Watch, destination.Directory,
										  destination.Filename, Actions::Moved,
										  FileSystem::canonicalSourcePath( source.Info.Filepath ) );
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
			deliverEvent( listener, i );
			if ( move != i )
				continue;
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
