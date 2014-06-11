#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/String.hpp>
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>

#ifdef EFSW_COMPILER_MSVC
	#ifndef S_ISDIR
	#define S_ISDIR(f) ((f)&_S_IFDIR)
	#endif

	#ifndef S_ISREG
	#define S_ISREG(f) ((f)&_S_IFREG)
	#endif

	#ifndef S_ISRDBL
	#define S_ISRDBL(f) ((f)&_S_IREAD)
	#endif
#else
	#include <unistd.h>

	#ifndef S_ISRDBL
	#define S_ISRDBL(f) ((f)&S_IRUSR)
	#endif
#endif

namespace efsw {

bool FileInfo::exists( const std::string& filePath )
{
	FileInfo fi( filePath );
	return fi.exists();
}

bool FileInfo::isLink( const std::string& filePath )
{
	FileInfo fi( filePath, true );
	return fi.isLink();
}

bool FileInfo::inodeSupported()
{
	#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	return true;
	#else
	return false;
	#endif
}

FileInfo::FileInfo() :
	ModificationTime(0),
	OwnerId(0),
	GroupId(0),
	Permissions(0),
	Inode(0)
{}

FileInfo::FileInfo( const std::string& filepath ) :
	Filepath( filepath ),
	ModificationTime(0),
	OwnerId(0),
	GroupId(0),
	Permissions(0),
	Inode(0)
{
	getInfo();
}

FileInfo::FileInfo( const std::string& filepath, bool linkInfo ) :
	Filepath( filepath ),
	ModificationTime(0),
	OwnerId(0),
	GroupId(0),
	Permissions(0),
	Inode(0)
{
	if ( linkInfo )
	{
		getRealInfo();
	}
	else
	{
		getInfo();
	}
}

void FileInfo::getInfo()
{
	/// Why i'm doing this? stat in mingw32 doesn't work for directories if the dir path ends with a path slash
	bool slashAtEnd = FileSystem::slashAtEnd( Filepath );

	if ( slashAtEnd )
	{
		FileSystem::dirRemoveSlashAtEnd( Filepath );
	}

	#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	struct stat st;
	int res = stat( Filepath.c_str(), &st );
	#else
	struct _stat st;
	int res = _wstat( String::fromUtf8( Filepath ).toWideString().c_str(), &st );
	#endif

	if ( 0 == res )
	{
		ModificationTime	= st.st_mtime;
		OwnerId				= st.st_uid;
		GroupId				= st.st_gid;
		Permissions			= st.st_mode;
		Inode				= st.st_ino;
	}

	if ( slashAtEnd )
	{
		FileSystem::dirAddSlashAtEnd( Filepath );
	}
}

void FileInfo::getRealInfo()
{
	bool slashAtEnd = FileSystem::slashAtEnd( Filepath );

	if ( slashAtEnd )
	{
		FileSystem::dirRemoveSlashAtEnd( Filepath );
	}

	#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	struct stat st;
	int res = lstat( Filepath.c_str(), &st );
	#else
	struct _stat st;
	int res = _wstat( String::fromUtf8( Filepath ).toWideString().c_str(), &st );
	#endif

	if ( 0 == res )
	{
		ModificationTime	= st.st_mtime;
		OwnerId				= st.st_uid;
		GroupId				= st.st_gid;
		Permissions			= st.st_mode;
		Inode				= st.st_ino;
	}

	if ( slashAtEnd )
	{
		FileSystem::dirAddSlashAtEnd( Filepath );
	}
}

bool FileInfo::operator==( const FileInfo& Other ) const
{
	return (	ModificationTime	== Other.ModificationTime &&
				OwnerId				== Other.OwnerId &&
				GroupId				== Other.GroupId &&
				Permissions			== Other.Permissions &&
				Inode				== Other.Inode
	);
}

bool FileInfo::isDirectory()
{
	return 0 != S_ISDIR(Permissions);
}

bool FileInfo::isRegularFile()
{
	return 0 != S_ISREG(Permissions);
}

bool FileInfo::isReadable()
{
	return 0 != S_ISRDBL(Permissions);
}

bool FileInfo::isLink()
{
#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	return S_ISLNK(Permissions);
#else
	return false;
#endif
}

std::string FileInfo::linksTo()
{
#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	if ( isLink() )
	{
		char * ch = realpath( Filepath.c_str(), NULL);

		std::string tstr( ch );

		free( ch );

		return tstr;
	}
#endif
	return std::string("");
}

bool FileInfo::exists()
{
#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	struct stat st;
	return stat( Filepath.c_str(), &st ) == 0;
#else
	struct _stat st;
	return _wstat( String::fromUtf8( Filepath ).toWideString().c_str(), &st ) == 0;
#endif
}

FileInfo& FileInfo::operator=( const FileInfo& Other )
{
	this->Filepath			= Other.Filepath;
	this->ModificationTime	= Other.ModificationTime;
	this->GroupId			= Other.GroupId;
	this->OwnerId			= Other.OwnerId;
	this->Permissions		= Other.Permissions;
	this->Inode				= Other.Inode;
	return *this;
}

bool FileInfo::sameInode( const FileInfo& Other ) const
{
	return inodeSupported() && Inode == Other.Inode;
}

bool FileInfo::operator!=( const FileInfo& Other ) const
{
	return !(*this == Other);
}

} 
