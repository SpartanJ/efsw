#ifndef EFSW_DIRWATCHERGENERIC_HPP
#define EFSW_DIRWATCHERGENERIC_HPP

#include <efsw/WatcherGeneric.hpp>
#include <efsw/FileInfo.hpp>
#include <map>

namespace efsw {

class DirWatcherGeneric
{
	public:
		typedef std::map<std::string, DirWatcherGeneric*> DirWatchMap;

		WatcherGeneric *	Watch;
		std::string			Directory;
		FileInfoMap			Files;
		DirWatchMap			Directories;
		FileInfo			DirInfo;
		bool				Recursive;

		DirWatcherGeneric( WatcherGeneric * ws, const std::string& directory, bool recursive );

		~DirWatcherGeneric();

		void watch();

		static bool isDir( const std::string& directory );

		void GetFiles( const std::string& directory, FileInfoMap& files );

		bool pathInWatches( std::string path );

		void addChilds();
	protected:
		bool			Deleted;
};

}

#endif
