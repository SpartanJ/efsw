#ifndef EFSW_FILESYSTEM_HPP
#define EFSW_FILESYSTEM_HPP

#include <efsw/FileInfo.hpp>
#include <efsw/base.hpp>

namespace efsw {

class FileSystem {
  public:
	static bool isDirectory( const std::string& path );

	/// Returns directory entries sorted by FileInfo::Filepath.
	static FileInfoList filesInfoFromPath( std::string path );

	static char getOSSlash();

	static bool slashAtEnd( const std::string& dir );

	static void dirAddSlashAtEnd( std::string& dir );

	/// Removes the final separator, unless the path is a root, whose separator is the path.
	static void dirRemoveSlashAtEnd( std::string& dir );

	/// @return Whether the path names a root: "/", or on Windows a lone separator, a drive root
	/// such as "C:\" and a share root such as "\\server\share\".
	static bool isRootPath( const std::string& path );

	static std::string fileNameFromPath( std::string filepath );

	static std::string pathRemoveFileName( std::string filepath );

	static std::string getLinkRealPath( std::string dir, std::string& curPath );

	static std::string precomposeFileName( const std::string& name );

	static bool isRemoteFS( const std::string& directory );

	static bool changeWorkingDirectory( const std::string& path );

	static std::string getCurrentWorkingDirectory();

	/// @return The path resolved, or an empty string when it cannot be, as when it does not
	/// exist.
	static std::string getRealPath( const std::string& path );

	/// Returns a canonicalized path when the final path components may no longer exist: the
	/// deepest existing directory is resolved, and the rest kept.
	static std::string canonicalSourcePath( const std::string& path );

#if EFSW_OS == EFSW_OS_WIN
	static std::wstring getWidePath( const std::string& path );
#endif
};

} // namespace efsw

#endif
