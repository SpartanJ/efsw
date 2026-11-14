#ifndef EFSW_FILEINFO_HPP
#define EFSW_FILEINFO_HPP

#include <efsw/base.hpp>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace efsw {

/// Device and inode of a file.
typedef std::pair<Uint64, Uint64> FileIdentity;

struct FileIdentityHash {
	size_t operator()( const FileIdentity& identity ) const {
		// Inodes differ far more than devices: mix the device into the inode's hash.
		return std::hash<Uint64>()( identity.second ^ ( identity.first * 0x9E3779B97F4A7C15ull ) );
	}
};

class FileInfo {
  public:
	static bool exists( const std::string& filePath );

	static bool isLink( const std::string& filePath );

	static bool inodeSupported();

	FileInfo();

	FileInfo( const std::string& filepath );

	FileInfo( const std::string& filepath, bool linkInfo );

	bool operator==( const FileInfo& Other ) const;

	bool operator!=( const FileInfo& Other ) const;

	bool isDirectory() const;

	bool isRegularFile() const;

	bool isReadable() const;

	bool sameInode( const FileInfo& Other ) const;

	FileIdentity identity() const { return FileIdentity( Device, Inode ); }

	bool isLink() const;

	std::string linksTo();

	bool exists();

	void getInfo();

	void getRealInfo();

	std::string Filepath;
	Uint64 ModificationTime;
	Uint64 Size;
	Uint32 OwnerId;
	Uint32 GroupId;
	Uint32 Permissions;
	Uint64 Device;
	Uint64 Inode;
	Uint64 LinkCount;
};

typedef std::vector<FileInfo> FileInfoList;
typedef std::vector<std::pair<std::string, FileInfo>> MovedList;

} // namespace efsw

#endif
