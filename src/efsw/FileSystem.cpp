#include <efsw/FileSystem.hpp>
#include <efsw/platform/platformimpl.hpp>

#include <sys/stat.h>

namespace efsw {

bool FileSystem::isDirectory( const std::string& path )
{
	struct stat st;
	int res = stat( path.c_str(), &st );

	if ( 0 == res )
	{
		return static_cast<bool>( S_ISDIR(st.st_mode) );
	}

	return false;
}

std::map<std::string, FileInfo> FileSystem::filesInfoFromPath( std::string path ) {
	dirAddSlashAtEnd( path );

	return Platform::FileSystem::filesInfoFromPath( path );
}

char FileSystem::getOSSlash()
{
	return Platform::FileSystem::getOSSlash();
}

void FileSystem::dirAddSlashAtEnd( std::string& dir )
{
	if ( dir.size() && dir[ dir.size() - 1 ] != getOSSlash() )
		dir += getOSSlash();
}

}
