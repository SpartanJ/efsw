#include <efsw/FileActionBatch.hpp>
#include <efsw/FileSystem.hpp>
#include <map>

namespace efsw {

FileActionBatch::Event::Event( WatchID watchid, const std::string& directory,
							   const std::string& filename, Action action,
							   const std::string& oldFilename, const FileInfo& fileInfo ) :
	Watch( watchid ),
	Directory( directory ),
	Filename( filename ),
	ActionType( action ),
	OldFilename( oldFilename ),
	Info( fileInfo ) {}

void FileActionBatch::clear() {
	mEvents.clear();
}

void FileActionBatch::add( WatchID watchid, const std::string& directory,
						   const std::string& filename, Action action,
						   const std::string& oldFilename, const FileInfo& fileInfo ) {
	mEvents.push_back( Event( watchid, directory, filename, action, oldFilename, fileInfo ) );
}

static bool metadataMatches( const FileInfo& source, const FileInfo& destination ) {
	return source.ModificationTime == destination.ModificationTime &&
		   source.Size == destination.Size && source.OwnerId == destination.OwnerId &&
		   source.GroupId == destination.GroupId && source.Permissions == destination.Permissions &&
		   source.isDirectory() == destination.isDirectory() &&
		   source.isRegularFile() == destination.isRegularFile();
}

static bool pathContains( const std::string& directory, const std::string& path ) {
	if ( directory.empty() || path.size() <= directory.size() ||
		 path.compare( 0, directory.size(), directory ) != 0 )
		return false;
	return directory[directory.size() - 1] == FileSystem::getOSSlash() ||
		   path[directory.size()] == FileSystem::getOSSlash();
}

static std::string canonicalSourcePath( const std::string& path ) {
	std::string directory( FileSystem::pathRemoveFileName( path ) );
	std::string canonicalDirectory( FileSystem::getRealPath( directory ) );
	if ( canonicalDirectory.empty() )
		return path;
	FileSystem::dirAddSlashAtEnd( canonicalDirectory );
	return canonicalDirectory + FileSystem::fileNameFromPath( path );
}

void FileActionBatch::dispatch( FileWatchListener* listener ) {
	if ( NULL == listener ) {
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
			listener->handleFileAction( destination.Watch, destination.Directory,
										destination.Filename, Actions::Moved,
										canonicalSourcePath( source.Info.Filepath ) );
		} else if ( !suppressed[i] ) {
			const Event& event = mEvents[i];
			listener->handleFileAction( event.Watch, event.Directory, event.Filename,
										event.ActionType, event.OldFilename );
		}
	}

	clear();
}

} // namespace efsw
