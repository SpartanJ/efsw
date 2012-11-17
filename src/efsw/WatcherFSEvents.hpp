#ifndef EFSW_WATCHERINOTIFY_HPP
#define EFSW_WATCHERINOTIFY_HPP

#include <efsw/FileWatcherImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <efsw/DirectorySnapshot.hpp>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <set>

namespace efsw {

class FileWatcherFSEvents;

class WatcherFSEvents : public Watcher
{
	public:
		typedef std::map<std::string, DirectorySnapshot> WatchMap;

		WatcherFSEvents();
		
		WatcherFSEvents( WatchID id, std::string directory, FileWatchListener * listener, bool recursive, WatcherFSEvents * parent = NULL );
		
		~WatcherFSEvents();

		bool inParentTree( WatcherFSEvents * parent );
		
		void init();

		void initAsync();
		
		void handleAction( const std::string& path, const Uint32& flags );
		
		void process();

		WatcherFSEvents * Parent;

		FileWatcherFSEvents * FWatcher;

		FSEventStreamRef FSStream;
	protected:
		void handleAddModDel( const Uint32 &flags, const std::string &path, std::string &dirPath, std::string &filePath );

		WatchMap DirectoriesSnaps;

		std::set<std::string> DirsChanged;
};

}

#endif

#endif
