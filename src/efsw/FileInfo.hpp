#ifndef EFSW_FILEINFO_HPP
#define EFSW_FILEINFO_HPP

#include <efsw/base.hpp>
#include <string>
#include <map>

namespace efsw {

class FileInfo
{
	public:
		FileInfo();

		FileInfo( const std::string& filepath );

		FileInfo( const std::string& filepath, bool linkInfo );

		bool operator==( const FileInfo& Other ) const;

		bool operator!=( const FileInfo& Other ) const;

		FileInfo& operator=( const FileInfo& Other );

		bool isDirectory();

		bool isRegularFile();

		bool isLink();

		std::string linksTo();

		bool exists();

		void getInfo();

		void getRealInfo();

		std::string		Filepath;
		Uint64			ModificationTime;
		Uint32			OwnerId;
		Uint32			GroupId;
		Uint32			Permissions;
		Uint64			Size;
};

typedef std::map<std::string, FileInfo> FileInfoMap;

}

#endif

