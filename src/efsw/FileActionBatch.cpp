#include <efsw/FileActionBatch.hpp>
#include <efsw/FileSystem.hpp>
#include <algorithm>
#include <iterator>
#include <set>

namespace efsw {

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

void FileActionBatch::scanned( Watcher* watcher ) {
	if ( !watcher->collectsSharedActions() )
		return;
	size_t begin = mSegments.empty() ? mReleased : mSegments.back().End;
	mSegments.push_back( { watcher, begin, mEvents.size() } );
}

void FileActionBatch::forget( Watcher* watcher ) {
	mHeld.erase( std::remove_if( mHeld.begin(), mHeld.end(),
								 [watcher]( const Event& event ) { return event.Owner == watcher; } ),
				 mHeld.end() );
}

void FileActionBatch::addHalves( HalvesMap& halves, size_t begin ) const {
	for ( size_t i = begin; i < mEvents.size(); i++ ) {
		const Event& event = mEvents[i];
		if ( event.Info.Inode == 0 )
			continue;
		if ( event.ActionType == Actions::Add )
			halves[FileIdentity( event.Info.Device, event.Info.Inode )].Added = true;
		else if ( event.ActionType == Actions::Delete )
			halves[FileIdentity( event.Info.Device, event.Info.Inode )].Deleted = true;
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
		if ( !later.empty() && segment.Owner->snapshotHasAny( later ) ) {
			size_t begin = mEvents.size();
			segment.Owner->watch();
			addHalves( halves, begin );
			mSegments.push_back( { segment.Owner, begin, mEvents.size() } );
			rescanned[s] = true;
		}
		for ( size_t i = segment.Begin; i < segment.End; i++ ) {
			const Event& event = mEvents[i];
			if ( event.ActionType != Actions::Add || event.Info.Inode == 0 )
				continue;
			FileIdentity identity( event.Info.Device, event.Info.Inode );
			if ( !halves[identity].paired() )
				later.insert( identity );
		}
	}
	return rescanned;
}

void FileActionBatch::holdMoveSources( const HalvesMap& halves,
									   const std::vector<bool>& rescanned ) {
	// For each watch, the position of its first held event.
	std::map<Watcher*, size_t> holdFrom;
	// Whether a watch scanned before the segment was not scanned again since. When the
	// segment has a Delete, that watch may only see the destination on its next scan.
	bool canWait = false;
	for ( size_t s = 0; s < rescanned.size(); s++ ) {
		const Segment& segment = mSegments[s];
		for ( size_t i = segment.Begin; canWait && i < segment.End; i++ ) {
			const Event& event = mEvents[i];
			if ( event.ActionType == Actions::Delete && event.Info.Inode != 0 &&
				 !halves.at( FileIdentity( event.Info.Device, event.Info.Inode ) ).paired() ) {
				holdFrom.emplace( segment.Owner, i );
				break;
			}
		}
		if ( !rescanned[s] )
			canWait = true;
	}
	if ( holdFrom.empty() )
		return;

	// Later events of a held watch are held too, including those of its rescan.
	std::vector<Watcher*> owners( mEvents.size(), NULL );
	for ( const Segment& segment : mSegments )
		for ( size_t i = segment.Begin; i < segment.End; i++ )
			owners[i] = segment.Owner;

	std::vector<Event> kept;
	kept.reserve( mEvents.size() );
	for ( size_t i = 0; i < mEvents.size(); i++ ) {
		auto hold = owners[i] ? holdFrom.find( owners[i] ) : holdFrom.end();
		if ( hold != holdFrom.end() && i >= hold->second ) {
			mEvents[i].Owner = owners[i];
			mHeld.push_back( std::move( mEvents[i] ) );
		} else {
			kept.push_back( std::move( mEvents[i] ) );
		}
	}
	mEvents.swap( kept );
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
	if ( unpaired ) {
		std::vector<bool> rescanned = rescanMoveSources( halves );
		holdMoveSources( halves, rescanned );
	}
	mSegments.clear();
	mReleased = 0;
	dispatch( NULL );
}

void FileActionBatch::dispatch( FileWatchListener* listener, bool detectMoves ) {
	if ( detectMoves ) {
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
			detectMoves = false;
	}
	if ( !detectMoves ) {
		for ( const Event& event : mEvents ) {
			FileWatchListener* target = event.Listener ? event.Listener : listener;
			if ( target )
				target->handleFileAction( event.Watch, event.Directory, event.Filename,
										  event.ActionType, event.OldFilename );
		}
		clear();
		return;
	}

	typedef std::pair<Uint64, Uint64> Identity;
	struct Candidates {
		std::vector<size_t> Created;
		std::vector<size_t> Deleted;
	};

	std::map<Identity, Candidates> candidates;
	for ( size_t i = 0; i < mEvents.size(); i++ ) {
		const Event& event = mEvents[i];
		if ( event.Info.Inode == 0 ||
			 ( event.ActionType != Actions::Add && event.ActionType != Actions::Delete ) )
			continue;

		Candidates& identity = candidates[Identity( event.Info.Device, event.Info.Inode )];
		if ( event.ActionType == Actions::Add )
			identity.Created.push_back( i );
		else
			identity.Deleted.push_back( i );
	}

	std::vector<bool> suppressed( mEvents.size(), false );
	std::map<size_t, size_t> moves;
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

		suppressed[entries.Deleted[0]] = true;
		suppressed[entries.Created[0]] = true;
		moves[entries.Created[0]] = entries.Deleted[0];

		if ( source.Info.isDirectory() ) {
			for ( size_t i = 0; i < mEvents.size(); i++ ) {
				if ( mEvents[i].ActionType == Actions::Delete &&
					 pathContains( source.Info.Filepath, mEvents[i].Info.Filepath ) )
					suppressed[i] = true;
				else if ( mEvents[i].ActionType == Actions::Add &&
						  pathContains( destination.Info.Filepath, mEvents[i].Info.Filepath ) )
					suppressed[i] = true;
			}
		}
	}

	for ( size_t i = 0; i < mEvents.size(); i++ ) {
		auto move = moves.find( i );
		if ( move != moves.end() ) {
			const Event& destination = mEvents[i];
			const Event& source = mEvents[move->second];
			FileWatchListener* target = destination.Listener ? destination.Listener : listener;
			if ( target )
				target->handleFileAction( destination.Watch, destination.Directory,
										  destination.Filename, Actions::Moved,
										  FileSystem::canonicalSourcePath( source.Info.Filepath ) );
		} else if ( !suppressed[i] ) {
			const Event& event = mEvents[i];
			FileWatchListener* target = event.Listener ? event.Listener : listener;
			if ( target )
				target->handleFileAction( event.Watch, event.Directory, event.Filename,
										  event.ActionType, event.OldFilename );
		}
	}

	clear();
}

} // namespace efsw
