#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>

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

	struct stat st;
	int res = stat( Filepath.c_str(), &st );

	if ( 0 == res )
	{
		ModificationTime	= st.st_mtime;
		OwnerId				= st.st_uid;
		GroupId				= st.st_gid;
		Permissions			= st.st_mode;
		#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
		Inode				= st.st_ino;
		#endif
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

	struct stat st;
	#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	int res = lstat( Filepath.c_str(), &st );
	#else
	int res = stat( Filepath.c_str(), &st );
	#endif

	if ( 0 == res )
	{
		ModificationTime	= st.st_mtime;
		OwnerId				= st.st_uid;
		GroupId				= st.st_gid;
		Permissions			= st.st_mode;
		#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
		Inode				= st.st_ino;
		#endif
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
	return S_ISDIR(Permissions);
}

bool FileInfo::isRegularFile()
{
	return S_ISREG(Permissions);
}

bool FileInfo::isReadable()
{
	return Permissions & S_IRUSR;
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
	struct stat st;
	return stat( Filepath.c_str(), &st ) == 0;
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
