#ifndef EFSW_FILESYSTEMIMPLWIN_HPP
#define EFSW_FILESYSTEMIMPLWIN_HPP

#include <efsw/base.hpp>
#include <efsw/String.hpp>
#include <efsw/FileInfo.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

#include <map>

namespace efsw { namespace Platform {

class FileSystem {
	public:
		static std::map<std::string, FileInfo> filesInfoFromPath( const std::string& path );

		static char getOSSlash();

		static bool isDirectory( const std::string& path );
};

}}

#endif

#endif
