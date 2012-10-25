#ifndef EFSW_FILEINFO_HPP
#define EFSW_FILEINFO_HPP

#include <string>

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

		std::string Filepath;
		unsigned long long ModificationTime;
		unsigned long OwnerId;
		unsigned long GroupId;
		unsigned long Permissions;
};

}

#endif

