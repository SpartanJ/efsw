#ifndef EFSW_WATCHERINOTIFY_HPP
#define EFSW_WATCHERINOTIFY_HPP

#include <efsw/FileWatcherImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <efsw/WatcherGeneric.hpp>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <set>

namespace efsw {

class FileWatcherFSEvents;

class WatcherFSEvents : public Watcher
{
	public:
		WatcherFSEvents();
		
		WatcherFSEvents( WatchID id, std::string directory, FileWatchListener * listener, bool recursive, WatcherFSEvents * parent = NULL );
		
		~WatcherFSEvents();

		void init();

		void initAsync();
		
		void handleAction( const std::string& path, const Uint32& flags );
		
		void process();

		FileWatcherFSEvents * FWatcher;

		FSEventStreamRef FSStream;
	protected:
		void handleAddModDel( const Uint32 &flags, const std::string &path, std::string &dirPath, std::string &filePath );

		WatcherGeneric * WatcherGen;

		bool initializedAsync;

		std::set<std::string> DirsChanged;
};

}

#endif

#endif
