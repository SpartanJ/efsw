#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>

#include <sys/stat.h>
#include <unistd.h>

namespace efsw {

FileInfo::FileInfo() :
	ModificationTime(0),
	OwnerId(0),
	GroupId(0),
	Permissions(0)
{}

FileInfo::FileInfo( const std::string& filepath ) :
	Filepath( filepath ),
	ModificationTime(0),
	OwnerId(0),
	GroupId(0),
	Permissions(0)
{
	getInfo();
}

FileInfo::FileInfo( const std::string& filepath, bool linkInfo ) :
	Filepath( filepath ),
	ModificationTime(0),
	OwnerId(0),
	GroupId(0),
	Permissions(0)
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
	struct stat st;
	int res = stat( Filepath.c_str(), &st );

	if ( 0 == res )
	{
		ModificationTime	= st.st_mtime;
		OwnerId				= st.st_uid;
		GroupId				= st.st_gid;
		Permissions			= st.st_mode;
		Size				= st.st_size;
	}
}

void FileInfo::getRealInfo()
{
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
		Size				= st.st_size;
	}
}

bool FileInfo::operator==( const FileInfo& Other ) const
{
	return (	ModificationTime	== Other.ModificationTime &&
				OwnerId				== Other.OwnerId &&
				GroupId				== Other.GroupId &&
				Permissions			== Other.Permissions &&
				Size				== Other.Size
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
		char * ch = new char[ Size + 1 ];

		ssize_t r = readlink( Filepath.c_str(), ch, Size + 1 );

		ch[ Size ] = '\0';

		std::string tstr( ch );

		efSAFE_DELETE_ARRAY( ch );

		if ( !( r < 0 || r > Size ) )
		{
			return tstr;
		}
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
	this->Size				= Other.Size;
	return *this;
}

bool FileInfo::operator!=( const FileInfo& Other ) const
{
	return !(*this == Other);
}

} 
