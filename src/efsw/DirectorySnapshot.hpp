#ifndef EFSW_DIRECTORYSNAPSHOT_HPP
#define EFSW_DIRECTORYSNAPSHOT_HPP

#include <efsw/DirectorySnapshotDiff.hpp>

namespace efsw {

class DirectorySnapshot
{
	public:
		FileInfo		DirectoryInfo;
		FileInfoMap		Files;

		void setDirectoryInfo( std::string directory );

		DirectorySnapshot();

		DirectorySnapshot( std::string directory );

		~DirectorySnapshot();

		void init( std::string directory );

		bool exists();

		DirectorySnapshotDiff scan();

		FileInfoMap::iterator nodeInFiles( FileInfo& fi );
	protected:
		void initFiles();

		void deleteAll( DirectorySnapshotDiff &Diff );
};

}

#endif

