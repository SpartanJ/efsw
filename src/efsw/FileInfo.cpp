#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/String.hpp>

#ifndef _DARWIN_FEATURE_64_BIT_INODE
#define _DARWIN_FEATURE_64_BIT_INODE
#endif

#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include <sys/stat.h>

#include <limits.h>
#include <stdlib.h>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32
#include <windows.h>
#endif

#ifdef EFSW_COMPILER_MSVC
#ifndef S_ISDIR
#define S_ISDIR( f ) ( ( f ) & _S_IFDIR )
#endif

#ifndef S_ISREG
#define S_ISREG( f ) ( ( f ) & _S_IFREG )
#endif

#ifndef S_ISRDBL
#define S_ISRDBL( f ) ( ( f ) & _S_IREAD )
#endif
#else
#include <unistd.h>

#ifndef S_ISRDBL
#define S_ISRDBL( f ) ( ( f ) & S_IRUSR )
#endif
#endif

namespace efsw {

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32
static void getWindowsFileIdentity( const std::string& filePath, Uint64& device, Uint64& inode,
									Uint64& linkCount ) {
	HANDLE handle = CreateFileW( FileSystem::getWidePath( filePath ).c_str(), FILE_READ_ATTRIBUTES,
								 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
								 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL );
	if ( handle == INVALID_HANDLE_VALUE )
		return;

	BY_HANDLE_FILE_INFORMATION info;
	if ( GetFileInformationByHandle( handle, &info ) ) {
		ULARGE_INTEGER fileIndex;
		fileIndex.HighPart = info.nFileIndexHigh;
		fileIndex.LowPart = info.nFileIndexLow;
		device = info.dwVolumeSerialNumber;
		inode = fileIndex.QuadPart;
		linkCount = info.nNumberOfLinks;
	}

	CloseHandle( handle );
}
#endif

bool FileInfo::exists( const std::string& filePath ) {
	FileInfo fi( filePath );
	return fi.exists();
}

bool FileInfo::isLink( const std::string& filePath ) {
	FileInfo fi( filePath, true );
	return fi.isLink();
}

bool FileInfo::inodeSupported() {
	return true;
}

FileInfo::FileInfo() :
	ModificationTime( 0 ),
	Size( 0 ),
	OwnerId( 0 ),
	GroupId( 0 ),
	Permissions( 0 ),
	Device( 0 ),
	Inode( 0 ),
	LinkCount( 0 ) {}

FileInfo::FileInfo( const std::string& filepath ) :
	Filepath( filepath ),
	ModificationTime( 0 ),
	OwnerId( 0 ),
	GroupId( 0 ),
	Permissions( 0 ),
	Device( 0 ),
	Inode( 0 ),
	LinkCount( 0 ) {
	getInfo();
}

FileInfo::FileInfo( const std::string& filepath, bool linkInfo ) :
	Filepath( filepath ),
	ModificationTime( 0 ),
	OwnerId( 0 ),
	GroupId( 0 ),
	Permissions( 0 ),
	Device( 0 ),
	Inode( 0 ),
	LinkCount( 0 ) {
	if ( linkInfo ) {
		getRealInfo();
	} else {
		getInfo();
	}
}

void FileInfo::getInfo() {
#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32
	if ( Filepath.size() == 3 && Filepath[1] == ':' && Filepath[2] == FileSystem::getOSSlash() ) {
		Filepath += FileSystem::getOSSlash();
	}
#endif

	/// Why i'm doing this? stat in mingw32 doesn't work for directories if the dir path ends with a
	/// path slash
	bool slashAtEnd = FileSystem::slashAtEnd( Filepath );

	if ( slashAtEnd ) {
		FileSystem::dirRemoveSlashAtEnd( Filepath );
	}

#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	struct stat st;
	int res = stat( Filepath.c_str(), &st );
#else
	struct _stat st;
	int res = _wstat( FileSystem::getWidePath( Filepath ).c_str(), &st );
#endif

	if ( 0 == res ) {
		ModificationTime = st.st_mtime;
		Size = st.st_size;
		OwnerId = st.st_uid;
		GroupId = st.st_gid;
		Permissions = st.st_mode;
		Device = st.st_dev;
		Inode = st.st_ino;
		LinkCount = st.st_nlink;

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32
		getWindowsFileIdentity( Filepath, Device, Inode, LinkCount );
#endif
	}

	if ( slashAtEnd ) {
		FileSystem::dirAddSlashAtEnd( Filepath );
	}
}

void FileInfo::getRealInfo() {
	bool slashAtEnd = FileSystem::slashAtEnd( Filepath );

	if ( slashAtEnd ) {
		FileSystem::dirRemoveSlashAtEnd( Filepath );
	}

#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	struct stat st;
	int res = lstat( Filepath.c_str(), &st );
#else
	struct _stat st;
	int res = _wstat( FileSystem::getWidePath( Filepath ).c_str(), &st );
#endif

	if ( 0 == res ) {
		ModificationTime = st.st_mtime;
		Size = st.st_size;
		OwnerId = st.st_uid;
		GroupId = st.st_gid;
		Permissions = st.st_mode;
		Device = st.st_dev;
		Inode = st.st_ino;
		LinkCount = st.st_nlink;

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32
		getWindowsFileIdentity( Filepath, Device, Inode, LinkCount );
#endif
	}

	if ( slashAtEnd ) {
		FileSystem::dirAddSlashAtEnd( Filepath );
	}
}

bool FileInfo::operator==( const FileInfo& Other ) const {
	return ( ModificationTime == Other.ModificationTime && Size == Other.Size &&
			 OwnerId == Other.OwnerId && GroupId == Other.GroupId &&
			 Permissions == Other.Permissions && Device == Other.Device && Inode == Other.Inode );
}

bool FileInfo::isDirectory() const {
	return 0 != S_ISDIR( Permissions );
}

bool FileInfo::isRegularFile() const {
	return 0 != S_ISREG( Permissions );
}

bool FileInfo::isReadable() const {
#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	static bool isRoot = getuid() == 0;
	return isRoot || 0 != S_ISRDBL( Permissions );
#else
	return 0 != S_ISRDBL( Permissions );
#endif
}

bool FileInfo::isLink() const {
#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	return S_ISLNK( Permissions );
#else
	return false;
#endif
}

std::string FileInfo::linksTo() {
#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	if ( isLink() ) {
		char* ch = realpath( Filepath.c_str(), NULL );

		if ( NULL != ch ) {
			std::string tstr( ch );

			free( ch );

			return tstr;
		}
	}
#endif
	return std::string( "" );
}

bool FileInfo::exists() {
	bool slashAtEnd = FileSystem::slashAtEnd( Filepath );

	if ( slashAtEnd ) {
		FileSystem::dirRemoveSlashAtEnd( Filepath );
	}

#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	struct stat st;
	int res = stat( Filepath.c_str(), &st );
#else
	struct _stat st;
	int res = _wstat( FileSystem::getWidePath( Filepath ).c_str(), &st );
#endif

	if ( slashAtEnd ) {
		FileSystem::dirAddSlashAtEnd( Filepath );
	}

	return 0 == res;
}

FileInfo& FileInfo::operator=( const FileInfo& Other ) {
	this->Filepath = Other.Filepath;
	this->Size = Other.Size;
	this->ModificationTime = Other.ModificationTime;
	this->GroupId = Other.GroupId;
	this->OwnerId = Other.OwnerId;
	this->Permissions = Other.Permissions;
	this->Device = Other.Device;
	this->Inode = Other.Inode;
	this->LinkCount = Other.LinkCount;
	return *this;
}

bool FileInfo::sameInode( const FileInfo& Other ) const {
	return inodeSupported() && Inode != 0 && Device == Other.Device && Inode == Other.Inode;
}

bool FileInfo::operator!=( const FileInfo& Other ) const {
	return !( *this == Other );
}

} // namespace efsw
