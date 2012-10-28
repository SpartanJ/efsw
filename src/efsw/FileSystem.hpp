#ifndef EFSW_FILESYSTEM_HPP
#define EFSW_FILESYSTEM_HPP

#include <efsw/base.hpp>
#include <efsw/FileInfo.hpp>
#include <map>

namespace efsw {

class FileSystem
{
	public:
		static bool isDirectory( const std::string& path );

		static std::map<std::string, FileInfo> filesInfoFromPath( std::string path );

		static char getOSSlash();

		static void dirAddSlashAtEnd( std::string& dir );

		static void dirRemoveSlashAtEnd( std::string& dir );

		static std::string fileNameFromPath( const std::string& filepath );

};

}

#endif
