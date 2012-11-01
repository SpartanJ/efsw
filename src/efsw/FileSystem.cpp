#include <efsw/FileSystem.hpp>
#include <efsw/platform/platformimpl.hpp>

#include <sys/stat.h>

namespace efsw {

bool FileSystem::isDirectory( const std::string& path )
{
	return Platform::FileSystem::isDirectory( path );
}

FileInfoMap FileSystem::filesInfoFromPath( std::string path ) {
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
		dir.push_back( getOSSlash() );
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

std::string FileSystem::pathRemoveFileName( const std::string& filepath )
{
	return filepath.substr( 0, filepath.find_last_of( getOSSlash() ) + 1 );
}

static void removeLastDir( std::string& path )
{
	bool ignoreFirst = ( path[ path.size() - 1 ] == FileSystem::getOSSlash() ) ? true : false;

	for ( size_t i = path.size() - 1; i >= 0; i-- )
	{
		if ( path.at(i) == FileSystem::getOSSlash() )
		{
			if ( ignoreFirst )
			{
				ignoreFirst = false;
			}
			else
			{
				path = path.substr( 0, i );
				break;
			}
		}
	}
}

static bool linksToCurrent( std::string& path )
{
	int cdots = 0;
	int cslash = 0;
	char c = 0;

	for ( size_t i = 0; i < path.size(); i++ )
	{
		c = path.at(i);

		if ( '.' == c )
		{
			cdots++;
		}
		else if ( FileSystem::getOSSlash() == c )
		{
			cslash++;
		}
	}

	if ( !cdots )
	{
		if (	cslash == 0 ||
			(	cslash == 1 && path[ path.size() - 1 ] == FileSystem::getOSSlash()  )
		)
		{
			return true;
		}
	}

	return false;
}

void FileSystem::realPath( std::string curdir, std::string& path )
{
	if ( path.size() == 1 )
	{
		if ( path.at(0) == getOSSlash() )
		{
			return;
		}
		else if ( path.at(0) == '.' )
		{
			path = curdir;
			return;
		}
	}

	if ( path[0] == '.' )
	{


		std::string npath( path );

		for ( size_t i = 0; i < path.size(); i++ )
		{
			if ( i + 1 < path.size() )
			{
				if ( path[i] == '.' && path[i+1] == '.' )
				{
					removeLastDir( curdir );

					size_t pos = npath.find_first_of( ".." );

					if ( pos + 3 < npath.size() )
					{
						npath = npath.substr( pos + 3 );
						i += 2;
					}
					else
					{
						npath = "";
					}
				}
				else if ( path[i] == '.' && path[i+1] == FileSystem::getOSSlash() )
				{
					size_t pos = npath.find_first_of( "./" );

					if ( pos + 2 < npath.size() )
					{
						npath = npath.substr( pos + 2 );
						i += 1;
					}
					else
					{
						npath = "";
					}
				}
			}
		}

		dirAddSlashAtEnd( curdir );

		path = curdir + npath;
	}
	else
	{
		if ( linksToCurrent( path ) )
		{
			path = curdir + path;
			return;
		}
	}
}

std::string FileSystem::getLinkRealPath( std::string dir, std::string& curPath )
{
	FileSystem::dirRemoveSlashAtEnd( dir );
	FileInfo fi( dir, true );

	/// Check with lstat and see if it's a link
	if ( fi.isLink() )
	{
		/// get the real path of the link
		std::string link( fi.linksTo() );

		/// get the current path of the directory without the link dir path
		curPath = FileSystem::pathRemoveFileName( dir );

		/// convert the link path to the real path
		FileSystem::realPath( curPath, link );

		/// ensure that ends with the os directory slash
		FileSystem::dirAddSlashAtEnd( link );

		return link;
	}

	/// if it's not a link return nothing
	return "";
}

}
