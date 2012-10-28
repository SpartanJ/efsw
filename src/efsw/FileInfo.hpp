#ifndef EFSW_FILEINFO_HPP
#define EFSW_FILEINFO_HPP

#include <string>
#include <map>

namespace efsw {

class FileInfo
{
	public:
		FileInfo();

		FileInfo( const std::string& filepath );

		bool operator==( const FileInfo& Other ) const;

		bool operator!=( const FileInfo& Other ) const;

		FileInfo& operator=( const FileInfo& Other );

		bool isDirectory();

		bool isRegularFile();

		bool exists();

		std::string Filepath;
		unsigned long long ModificationTime;
		unsigned long OwnerId;
		unsigned long GroupId;
		unsigned long Permissions;
};

typedef std::map<std::string, FileInfo> FileInfoMap;

}

#endif

