#include <efsw/FileSystem.hpp>
#include <efsw/platform/platformimpl.hpp>

#include <sys/stat.h>

namespace efsw {

bool FileSystem::isDirectory( const std::string& path )
{
	return Platform::FileSystem::isDirectory( path );
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
	{
		dir += getOSSlash();
	}
}

void FileSystem::dirRemoveSlashAtEnd( std::string& dir )
{
	if ( dir.size() && dir[ dir.size() - 1 ] == getOSSlash() )
	{
		dir.erase( dir.size() - 1 );
	}
}

std::string FileSystem::fileNameFromPath( const std::string& filepath )
{
	return filepath.substr( filepath.find_last_of( getOSSlash() ) + 1 );
}

}
