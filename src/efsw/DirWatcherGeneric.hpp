#ifndef EFSW_DIRWATCHERGENERIC_HPP
#define EFSW_DIRWATCHERGENERIC_HPP

#include <efsw/DirectorySnapshot.hpp>
#include <efsw/FileInfo.hpp>
#include <efsw/WatcherGeneric.hpp>
#include <map>

namespace efsw {

class DirWatcherGeneric {
  public:
	typedef std::map<std::string, DirWatcherGeneric*> DirWatchMap;

	DirWatcherGeneric* Parent;
	WatcherGeneric* Watch;
	DirectorySnapshot DirSnap;
	DirWatchMap Directories;
	bool Recursive;

	DirWatcherGeneric( DirWatcherGeneric* parent, WatcherGeneric* ws, const std::string& directory,
					   bool recursive, bool reportNewFiles = false );

	~DirWatcherGeneric();

	void watch( bool reportOwnChange = false );

	void watchDir( std::string& dir );

	static bool isDir( const std::string& directory );

	bool pathInWatches( std::string path );

	/// @return True when the snapshot of this directory or a watched subdirectory lists a file
	/// with one of the identities.
	bool snapshotHasAny( const std::vector<FileIdentity>& identities ) const;

	void addChildren( bool reportNewFiles = true );

	DirWatcherGeneric* findDirWatcher( std::string dir );

	/// @return The watcher of the directory, this one or one below it, or NULL.
	DirWatcherGeneric* findTree( const std::string& directory );

	/// Appends the entries of the snapshots of this directory and those below it.
	void collectEntries( std::vector<const FileInfo*>& entries ) const;

	/// Moves the snapshot lists of this directory and those below it into lists.
	void takeSnapshots( std::vector<FileInfoList>& lists );

	DirWatcherGeneric* findDirWatcherFast( std::string dir );

	/// Sets the directory, after it moved, and the directories of the tree below it.
	void resetDirectory( std::string directory );

  protected:
	bool Deleted;

	DirWatcherGeneric* createDirectory( std::string newdir );

	void removeDirectory( std::string dir );

	void moveDirectory( std::string oldDir, std::string newDir );


	void handleAction( const std::string& filename, unsigned long action,
					   const std::string& oldFilename, const FileInfo& fileInfo );
};

} // namespace efsw

#endif
