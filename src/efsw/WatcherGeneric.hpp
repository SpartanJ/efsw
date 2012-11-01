#ifndef EFSW_WATCHERGENERIC_HPP
#define EFSW_WATCHERGENERIC_HPP

#include <efsw/FileWatcherImpl.hpp>

namespace efsw
{

class DirWatcherGeneric;

class WatcherGeneric : public Watcher
{
	public:
		FileWatcherImpl *		WatcherImpl;
		DirWatcherGeneric *		DirWatch;
		DirWatcherGeneric *		CurDirWatch;

		WatcherGeneric( WatchID id, const std::string& directory, FileWatchListener * fwl, FileWatcherImpl * fw, bool recursive );

		~WatcherGeneric();

		void watch();

		bool isPath( std::string path );
};

}

#endif
