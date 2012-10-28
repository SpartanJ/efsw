#include <efsw/FileInfo.hpp>
#include <sys/stat.h>

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
	struct stat st;
	int res = stat( filepath.c_str(), &st );

	if ( 0 == res )
	{
		ModificationTime	= st.st_mtime;
		OwnerId				= st.st_uid;
		GroupId				= st.st_gid;
		Permissions			= st.st_mode;
	}
}

bool FileInfo::operator==( const FileInfo& Other ) const
{
	return (	ModificationTime	== Other.ModificationTime &&
				OwnerId				== Other.OwnerId &&
				GroupId				== Other.GroupId &&
				Permissions			== Other.Permissions
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

bool FileInfo::exists()
{
	struct stat st;
	return stat( Filepath.c_str(), &st ) == 0;
}

FileInfo& FileInfo::operator=( const FileInfo& Other )
{
	this->Filepath = Other.Filepath;
	this->ModificationTime = Other.ModificationTime;
	this->GroupId = Other.GroupId;
	this->OwnerId = Other.OwnerId;
	this->Permissions = Other.Permissions;
	return *this;
}

bool FileInfo::operator!=( const FileInfo& Other ) const
{
	return !(*this == Other);
}

} 
