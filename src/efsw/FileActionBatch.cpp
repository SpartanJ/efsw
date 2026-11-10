#include <efsw/FileActionBatch.hpp>
#include <efsw/FileSystem.hpp>
#include <map>

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

bool FileActionBatch::lastUnpairedHalf( size_t& position ) const {
	typedef std::pair<Uint64, Uint64> Identity;
	struct Halves {
		bool Added{ false };
		bool Deleted{ false };
	};
	std::map<Identity, Halves> identities;
	for ( const Event& event : mEvents ) {
		if ( event.Info.Inode == 0 )
			continue;
		if ( event.ActionType == Actions::Add )
			identities[Identity( event.Info.Device, event.Info.Inode )].Added = true;
		else if ( event.ActionType == Actions::Delete )
			identities[Identity( event.Info.Device, event.Info.Inode )].Deleted = true;
	}
	for ( size_t i = mEvents.size(); i-- > 0; ) {
		const Event& event = mEvents[i];
		if ( event.Info.Inode == 0 ||
			 ( event.ActionType != Actions::Add && event.ActionType != Actions::Delete ) )
			continue;
		const Halves& halves = identities[Identity( event.Info.Device, event.Info.Inode )];
		if ( !halves.Added || !halves.Deleted ) {
			position = i;
			return true;
		}
	}
	return false;
}

void FileActionBatch::rescanForUnpairedHalf(
	const std::vector<std::pair<Watcher*, size_t>>& scanned ) {
	size_t unpaired;
	if ( !lastUnpairedHalf( unpaired ) )
		return;
	for ( const auto& entry : scanned ) {
		// The first watch whose scan ended past the event is the one that collected it.
		if ( unpaired < entry.second )
			break;
		if ( entry.first->collectsSharedActions() )
			entry.first->watch();
	}
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
