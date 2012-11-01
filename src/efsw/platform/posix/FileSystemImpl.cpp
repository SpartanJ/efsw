#include <efsw/platform/posix/FileSystemImpl.hpp>

#if defined( EFSW_PLATFORM_POSIX )

#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>
#include <dirent.h>
#include <cstring>
#include <sys/stat.h>
#include <cstdlib>

namespace efsw { namespace Platform {

FileInfoMap FileSystem::filesInfoFromPath( const std::string& path )
{
	FileInfoMap files;

	DIR *dp;
	struct dirent *dirp;

	if( ( dp = opendir( path.c_str() ) ) == NULL)
		return files;

	while ( ( dirp = readdir(dp) ) != NULL)
	{
		if ( strcmp( dirp->d_name, ".." ) != 0 && strcmp( dirp->d_name, "." ) != 0 )
		{
			std::string name( dirp->d_name );
			std::string fpath( path + name );

			files[ name ] = FileInfo( fpath );
		}
	}

	closedir(dp);

	return files;
}

char FileSystem::getOSSlash()
{
	return '/';
}

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

}}

#endif
