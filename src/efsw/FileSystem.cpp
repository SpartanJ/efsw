#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <efsw/FileSystem.hpp>
#include <efsw/platform/platformimpl.hpp>

#if EFSW_OS == EFSW_OS_MACOSX
#include <CoreFoundation/CoreFoundation.h>
#endif

#if EFSW_OS == EFSW_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if __cplusplus >= 201703L || _MSVC_LANG >= 201703L
#define USE_FILESYSTEM_PATH_CONVERSION
#include <filesystem>
#endif
#endif

namespace efsw {

bool FileSystem::isDirectory( const std::string& path ) {
	return Platform::FileSystem::isDirectory( path );
}

FileInfoList FileSystem::filesInfoFromPath( std::string path ) {
	dirAddSlashAtEnd( path );

	FileInfoList files = Platform::FileSystem::filesInfoFromPath( path );
	std::sort( files.begin(), files.end(), []( const FileInfo& left, const FileInfo& right ) {
		return left.Filepath < right.Filepath;
	} );
	return files;
}

char FileSystem::getOSSlash() {
	return Platform::FileSystem::getOSSlash();
}

bool FileSystem::slashAtEnd( const std::string& dir ) {
	return ( dir.size() && dir[dir.size() - 1] == getOSSlash() );
}

void FileSystem::dirAddSlashAtEnd( std::string& dir ) {
	if ( dir.size() >= 1 && dir[dir.size() - 1] != getOSSlash() ) {
		dir.push_back( getOSSlash() );
	}
}

void FileSystem::dirRemoveSlashAtEnd( std::string& dir ) {
	if ( dir.size() >= 1 && dir[dir.size() - 1] == getOSSlash() && !isRootPath( dir ) ) {
		dir.erase( dir.size() - 1 );
	}
}

bool FileSystem::isRootPath( const std::string& path ) {
	const char slash = getOSSlash();
	if ( path.size() == 1 )
		return path[0] == slash;
#if EFSW_OS == EFSW_OS_WIN
	if ( path.size() == 3 && path[1] == ':' && path[2] == slash )
		return true;
	// A share root: the separator after the share name ends the path.
	if ( path.size() > 4 && path[0] == slash && path[1] == slash &&
		 path[path.size() - 1] == slash ) {
		const size_t server = path.find( slash, 2 );
		return server != std::string::npos && server > 2 && server + 1 < path.size() - 1 &&
			   path.find( slash, server + 1 ) == path.size() - 1;
	}
#endif
	return false;
}

std::string FileSystem::fileNameFromPath( std::string filepath ) {
	dirRemoveSlashAtEnd( filepath );

	size_t pos = filepath.find_last_of( getOSSlash() );

	if ( pos != std::string::npos ) {
		return filepath.substr( pos + 1 );
	}

	return filepath;
}

std::string FileSystem::pathRemoveFileName( std::string filepath ) {
	dirRemoveSlashAtEnd( filepath );

	size_t pos = filepath.find_last_of( getOSSlash() );

	if ( pos != std::string::npos ) {
		return filepath.substr( 0, pos + 1 );
	}

	return filepath;
}

std::string FileSystem::getLinkRealPath( std::string dir, std::string& curPath ) {
	FileSystem::dirRemoveSlashAtEnd( dir );
	FileInfo fi( dir, true );

	/// Check with lstat and see if it's a link
	if ( fi.isLink() ) {
		/// get the real path of the link
		std::string link( fi.linksTo() );

		/// get the current path of the directory without the link dir path
		curPath = FileSystem::pathRemoveFileName( dir );

		/// ensure that ends with the os directory slash
		FileSystem::dirAddSlashAtEnd( link );

		return link;
	}

	/// if it's not a link return nothing
	return "";
}

std::string FileSystem::precomposeFileName( const std::string& name ) {
#if EFSW_OS == EFSW_OS_MACOSX
	CFStringRef cfStringRef =
		CFStringCreateWithCString( kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8 );
	CFMutableStringRef cfMutable = CFStringCreateMutableCopy( NULL, 0, cfStringRef );

	CFStringNormalize( cfMutable, kCFStringNormalizationFormC );

	const char* c_str = CFStringGetCStringPtr( cfMutable, kCFStringEncodingUTF8 );
	if ( c_str != NULL ) {
		std::string result( c_str );
		CFRelease( cfStringRef );
		CFRelease( cfMutable );
		return result;
	}
	CFIndex length = CFStringGetLength( cfMutable );
	CFIndex maxSize = CFStringGetMaximumSizeForEncoding( length, kCFStringEncodingUTF8 );
	if ( maxSize == kCFNotFound ) {
		CFRelease( cfStringRef );
		CFRelease( cfMutable );
		return std::string();
	}

	std::string result( maxSize + 1, '\0' );
	if ( CFStringGetCString( cfMutable, &result[0], result.size(), kCFStringEncodingUTF8 ) ) {
		result.resize( std::strlen( result.c_str() ) );
		CFRelease( cfStringRef );
		CFRelease( cfMutable );
	} else {
		result.clear();
	}
	return result;
#else
	return name;
#endif
}

bool FileSystem::isRemoteFS( const std::string& directory ) {
	return Platform::FileSystem::isRemoteFS( directory );
}

bool FileSystem::changeWorkingDirectory( const std::string& directory ) {
	return Platform::FileSystem::changeWorkingDirectory( directory );
}

std::string FileSystem::getCurrentWorkingDirectory() {
	return Platform::FileSystem::getCurrentWorkingDirectory();
}

std::string FileSystem::getRealPath( const std::string& path ) {
	std::string realPath;
#if defined( EFSW_PLATFORM_POSIX )
	// On failure, the buffer holds what was resolved so far, or nothing defined.
	char dir[PATH_MAX];
	if ( NULL != realpath( path.c_str(), &dir[0] ) )
		realPath = std::string( dir );
#elif EFSW_OS == EFSW_OS_WIN
	wchar_t dir[_MAX_PATH + 1];
	const DWORD length =
		GetFullPathNameW( getWidePath( path ).c_str(), _MAX_PATH, &dir[0], nullptr );
	if ( 0 != length && length < _MAX_PATH )
		realPath = String( dir ).toUtf8();
#else
#warning FileSystem::getRealPath() not implemented on this platform.
#endif
	return realPath;
}

std::string FileSystem::canonicalSourcePath( const std::string& path ) {
	// The source may be gone with its directory, moved or deleted since: the deepest directory
	// that still exists is resolved, and the rest of the path kept.
	std::string directory( FileSystem::pathRemoveFileName( path ) );
	std::string rest( FileSystem::fileNameFromPath( path ) );
	while ( !directory.empty() ) {
		std::string canonicalDirectory( FileSystem::getRealPath( directory ) );
		if ( !canonicalDirectory.empty() ) {
			FileSystem::dirAddSlashAtEnd( canonicalDirectory );
			return canonicalDirectory + rest;
		}
		const std::string parent( FileSystem::pathRemoveFileName( directory ) );
		if ( parent == directory )
			break;
		rest = FileSystem::fileNameFromPath( directory ) + getOSSlash() + rest;
		directory = parent;
	}
	return path;
}

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32
std::wstring FileSystem::getWidePath( const std::string& path ) {
#if defined( USE_FILESYSTEM_PATH_CONVERSION )
	return std::filesystem::u8path( path ).wstring();
#else
	return String::fromUtf8( path ).toWideString();
#endif
}
#endif

} // namespace efsw
