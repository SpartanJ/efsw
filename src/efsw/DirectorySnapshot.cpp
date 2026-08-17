#include <algorithm>
#include <efsw/DirectorySnapshot.hpp>
#include <efsw/FileSystem.hpp>

namespace efsw {

namespace {

struct FilePathLess {
	bool operator()( const FileInfo& left, const std::string& right ) const {
		return left.Filepath < right;
	}
};

FileInfoList::iterator lowerBoundFile( FileInfoList& files, const std::string& path ) {
	return std::lower_bound( files.begin(), files.end(), path, FilePathLess() );
}

FileInfoList::const_iterator lowerBoundFile( const FileInfoList& files, const std::string& path ) {
	return std::lower_bound( files.begin(), files.end(), path, FilePathLess() );
}

FileInfoList::iterator findFile( FileInfoList& files, const std::string& path ) {
	auto it = lowerBoundFile( files, path );
	return it != files.end() && it->Filepath == path ? it : files.end();
}

FileInfoList::const_iterator findFile( const FileInfoList& files, const std::string& path ) {
	auto it = lowerBoundFile( files, path );
	return it != files.end() && it->Filepath == path ? it : files.end();
}

void removeUnsupportedFiles( FileInfoList& files ) {
	files.erase( std::remove_if( files.begin(), files.end(),
								 []( const FileInfo& file ) {
									 return !file.isRegularFile() && !file.isDirectory();
								 } ),
				 files.end() );
}

} // namespace

DirectorySnapshot::DirectorySnapshot() {}

DirectorySnapshot::DirectorySnapshot( std::string directory ) {
	init( directory );
}

DirectorySnapshot::~DirectorySnapshot() {}

void DirectorySnapshot::init( std::string directory ) {
	setDirectoryInfo( directory );
	initFiles();
}

bool DirectorySnapshot::exists() {
	return DirectoryInfo.exists();
}

void DirectorySnapshot::deleteAll( DirectorySnapshotDiff& Diff ) {
	for ( const auto& file : Files ) {
		if ( file.isDirectory() ) {
			Diff.DirsDeleted.push_back( file );
		} else {
			Diff.FilesDeleted.push_back( file );
		}
	}

	Files.clear();
}

void DirectorySnapshot::setDirectoryInfo( std::string directory ) {
	std::string newDirectory( directory );
	FileSystem::dirAddSlashAtEnd( newDirectory );
	for ( auto& file : Files ) {
		file.Filepath = newDirectory + FileSystem::fileNameFromPath( file.Filepath );
	}
	DirectoryInfo = FileInfo( directory );
}

void DirectorySnapshot::initFiles() {
	Files = FileSystem::filesInfoFromPath( DirectoryInfo.Filepath );
	removeUnsupportedFiles( Files );
}

DirectorySnapshotDiff DirectorySnapshot::scan() {
	DirectorySnapshotDiff Diff;

	Diff.clear();

	FileInfo curFI( DirectoryInfo.Filepath );

	Diff.DirChanged = DirectoryInfo != curFI;

	if ( Diff.DirChanged ) {
		DirectoryInfo = curFI;
	}

	/// If the directory was erased, create the events for files and directories deletion
	if ( !curFI.exists() ) {
		deleteAll( Diff );

		return Diff;
	}

	FileInfoList files = FileSystem::filesInfoFromPath( DirectoryInfo.Filepath );
	removeUnsupportedFiles( files );

	if ( files.empty() && Files.empty() ) {
		return Diff;
	}

	for ( const auto& file : files ) {
		/// File existed before?
		auto fiIt = findFile( Files, file.Filepath );

		if ( fiIt != Files.end() ) {
			/// File changed?
			if ( *fiIt != file ) {
				/// handle modified event
				if ( file.isDirectory() ) {
					Diff.DirsModified.push_back( file );
				} else {
					Diff.FilesModified.push_back( file );
				}
			}
		} else {
			/// Check if the same inode already existed
			auto fit = nodeInFiles( file, files );
			if ( fit != Files.end() ) {
				std::string oldFile( FileSystem::fileNameFromPath( fit->Filepath ) );

				/// Delete the old file name
				Files.erase( fit );

				if ( file.isDirectory() ) {
					Diff.DirsMoved.push_back( std::make_pair( oldFile, file ) );
				} else {
					Diff.FilesMoved.push_back( std::make_pair( oldFile, file ) );
				}
			} else {
				if ( file.isDirectory() ) {
					Diff.DirsCreated.push_back( file );
				} else {
					Diff.FilesCreated.push_back( file );
				}
			}
		}
	}

	/// The files or directories that are missing from the current scan were deleted
	for ( const auto& file : Files ) {
		if ( findFile( files, file.Filepath ) == files.end() ) {
			if ( file.isDirectory() ) {
				Diff.DirsDeleted.push_back( file );
			} else {
				Diff.FilesDeleted.push_back( file );
			}
		}
	}

	/// The current scan becomes the next snapshot without copying either list
	Files.swap( files );

	return Diff;
}

FileInfoList::iterator DirectorySnapshot::nodeInFiles( const FileInfo& fi,
													   const FileInfoList& currentFiles ) {
	if ( FileInfo::inodeSupported() ) {
		for ( auto it = Files.begin(); it != Files.end(); ++it ) {
			if ( findFile( currentFiles, it->Filepath ) == currentFiles.end() &&
				 it->sameInode( fi ) && *it == fi && it->Filepath != fi.Filepath ) {
				return it;
			}
		}
	}

	return Files.end();
}

void DirectorySnapshot::addFile( std::string path ) {
	FileInfo file( path );
	auto it = lowerBoundFile( Files, path );
	if ( it != Files.end() && it->Filepath == path )
		*it = file;
	else
		Files.insert( it, file );
}

void DirectorySnapshot::removeFile( std::string path ) {
	auto it = findFile( Files, path );

	if ( Files.end() != it ) {
		Files.erase( it );
	}
}

void DirectorySnapshot::moveFile( std::string oldPath, std::string newPath ) {
	removeFile( oldPath );
	addFile( newPath );
}

void DirectorySnapshot::updateFile( std::string path ) {
	addFile( path );
}

} // namespace efsw
