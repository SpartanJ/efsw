#include <efsw/FileActionBatch.hpp>
#include <efsw/FileSystem.hpp>
#include <algorithm>
#include <iterator>
#include <set>

namespace efsw {

// Required before C++17, where Moves::None is odr-used by assign().
constexpr size_t FileActionBatch::Moves::None;

FileActionBatch::Event::Event( WatchID watchid, const std::string& directory,
							   const std::string& filename, Action action,
							   const std::string& oldFilename, const FileInfo& fileInfo,
							   FileWatchListener* listener ) :
	Watch( watchid ),
	Directory( directory ),
	Filename( filename ),
	ActionType( action ),
	OldFilename( oldFilename ),
	Info( fileInfo ),
	Listener( listener ) {}

void FileActionBatch::clear() {
	mEvents.clear();
}

void FileActionBatch::add( WatchID watchid, const std::string& directory,
						   const std::string& filename, Action action,
						   const std::string& oldFilename, const FileInfo& fileInfo,
						   FileWatchListener* listener ) {
	mEvents.emplace_back( watchid, directory, filename, action, oldFilename, fileInfo, listener );
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

static bool pathContains( const std::string& directory, const std::string& path ) {
	if ( directory.empty() || path.size() <= directory.size() ||
		 path.compare( 0, directory.size(), directory ) != 0 )
		return false;
	return directory[directory.size() - 1] == FileSystem::getOSSlash() ||
		   path[directory.size()] == FileSystem::getOSSlash();
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
		mEvents[i].Owner = owner;
	mSegments.push_back( { owner, begin, mEvents.size() } );
}

void FileActionBatch::scanned( Watcher* watcher ) {
	if ( !watcher->collectsSharedActions() )
		return;
	addSegment( watcher, mSegments.empty() ? mReleased : mSegments.back().End );
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
	std::map<Watcher*, size_t> holdFrom;
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
	deliver( NULL, hasMoves ? &moves : NULL );
}

bool FileActionBatch::matchMoves( Moves& moves ) const {
	moves.MoveOf.assign( mEvents.size(), Moves::None );
	moves.SourceOf.clear();
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

	std::map<FileIdentity, Candidates> candidates;
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

	for ( const auto& identity : candidates ) {
		const Candidates& entries = identity.second;
		if ( entries.Created.size() != 1 || entries.Deleted.size() != 1 )
			continue;

		const Event& source = mEvents[entries.Deleted[0]];
		const Event& destination = mEvents[entries.Created[0]];
		if ( source.Directory == destination.Directory ||
			 !source.Info.sameInode( destination.Info ) ||
			 ( source.Info.isRegularFile() &&
			   ( source.Info.LinkCount != 1 || destination.Info.LinkCount != 1 ) ) ||
			 !metadataMatches( source.Info, destination.Info ) )
			continue;

		moves.MoveOf[entries.Deleted[0]] = entries.Created[0];
		moves.MoveOf[entries.Created[0]] = entries.Created[0];
		moves.SourceOf[entries.Created[0]] = entries.Deleted[0];

		if ( source.Info.isDirectory() ) {
			for ( size_t i = 0; i < mEvents.size(); i++ ) {
				if ( moves.MoveOf[i] != Moves::None )
					continue;
				if ( ( mEvents[i].ActionType == Actions::Delete &&
					   pathContains( source.Info.Filepath, mEvents[i].Info.Filepath ) ) ||
					 ( mEvents[i].ActionType == Actions::Add &&
					   pathContains( destination.Info.Filepath, mEvents[i].Info.Filepath ) ) )
					moves.MoveOf[i] = entries.Created[0];
			}
		}
	}
	return !moves.SourceOf.empty();
}

void FileActionBatch::deliver( FileWatchListener* listener, const Moves* moves ) {
	// Indexed, and the flags read at each step: a listener may cancel events of a removed watch.
	for ( size_t i = 0; i < mEvents.size(); i++ ) {
		const Event& event = mEvents[i];
		if ( event.Canceled )
			continue;
		FileWatchListener* target = event.Listener ? event.Listener : listener;
		if ( !target )
			continue;
		if ( moves ) {
			auto move = moves->SourceOf.find( i );
			if ( move != moves->SourceOf.end() ) {
				const Event& source = mEvents[move->second];
				target->handleFileAction( event.Watch, event.Directory, event.Filename,
										  Actions::Moved,
										  FileSystem::canonicalSourcePath( source.Info.Filepath ) );
				continue;
			}
			if ( moves->MoveOf[i] != Moves::None )
				continue;
		}
		target->handleFileAction( event.Watch, event.Directory, event.Filename, event.ActionType,
								  event.OldFilename );
	}
	clear();
}

void FileActionBatch::dispatch( FileWatchListener* listener, bool detectMoves ) {
	Moves moves;
	bool hasMoves = detectMoves && matchMoves( moves );
	deliver( listener, hasMoves ? &moves : NULL );
}

} // namespace efsw
