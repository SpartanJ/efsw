#ifndef EFSW_DIRECTORYSNAPSHOT_HPP
#define EFSW_DIRECTORYSNAPSHOT_HPP

#include <efsw/DirectorySnapshotDiff.hpp>

namespace efsw {

class DirectorySnapshot {
	public:
	FileInfo DirectoryInfo;
	/// Kept sorted by FileInfo::Filepath for binary lookup.
	FileInfoList Files;

	void setDirectoryInfo( std::string directory );

	DirectorySnapshot();

	DirectorySnapshot( std::string directory );

	~DirectorySnapshot();

	void init( std::string directory );

	bool exists();

	DirectorySnapshotDiff scan();

	FileInfoList::iterator nodeInFiles( const FileInfo& fi, const FileInfoList& currentFiles );

	void addFile( std::string path );

	void removeFile( std::string path );

	void moveFile( std::string oldPath, std::string newPath );

	void updateFile( std::string path );

  protected:
	void initFiles();

	void deleteAll( DirectorySnapshotDiff& Diff );
};

} // namespace efsw

#endif
