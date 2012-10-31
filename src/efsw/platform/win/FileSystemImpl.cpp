#include <efsw/platform/win/FileSystemImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef EFSW_COMPILER_MSVC
#include <dirent.h>
#endif

namespace efsw { namespace Platform {

FileInfoMap FileSystem::filesInfoFromPath( const std::string& path )
{
	FileInfoMap files;

#ifdef EFSW_COMPILER_MSVC
	#ifdef UNICODE
		/// Unicode support just for this? MMHH...
		String tpath( String::fromUtf8( path ) );

		if ( tpath[ tpath.size() - 1 ] == '/' || tpath[ tpath.size() - 1 ] == '\\' ) {
			tpath += "*";
		} else {
			tpath += "\\*";
		}

		WIN32_FIND_DATA findFileData;
		HANDLE hFind = FindFirstFile( (LPCWSTR)tpath.toWideString().c_str(), &findFileData );

		if( hFind != INVALID_HANDLE_VALUE ) {
			String name( findFileData.cFileName );
			std::string fpath( path + name.toUtf8() );

			if ( name != "." && name != ".." )
			{
				files[ name.toUtf8() ] = FileInfo( fpath );
			}

			while( FindNextFile(hFind, &findFileData ) ) {
				name = String( findFileData.cFileName );
				fpath = path + name.toUtf8();

				if ( name != "." && name != ".." )
				{
					files[ name.toUtf8() ] = FileInfo( fpath );
				}
			}

			FindClose( hFind );
		}
	#else
		std::string tpath( path );

		if ( tpath[ tpath.size() - 1 ] == '/' || tpath[ tpath.size() - 1 ] == '\\' ) {
				tpath += "*";
		} else {
				tpath += "\\*";
		}

		WIN32_FIND_DATA findFileData;
		HANDLE hFind = FindFirstFile( (LPCTSTR) tpath.c_str(), &findFileData );

		if( hFind != INVALID_HANDLE_VALUE ) {
			std::string name( findFileData.cFileName );
			std::string fpath( path + name );

			if ( name != "." && name != ".." )
			{
				files[ name ] = FileInfo( fpath );
			}

			while( FindNextFile(hFind, &findFileData ) ) {
				name = std::string( findFileData.cFileName );
				fpath = path + name;

				if ( name != "." && name != ".." )
				{
					files[ name ] = FileInfo( fpath );
				}
			}

			FindClose( hFind );
		}
	#endif

	return files;
#else
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
#endif
}

char FileSystem::getOSSlash()
{
	return '\\';
}

bool FileSystem::isDirectory( const std::string& path )
{
	return GetFileAttributes( (LPCTSTR) path.c_str() ) == FILE_ATTRIBUTE_DIRECTORY;
}


}}

#endif
