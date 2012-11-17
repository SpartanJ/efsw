#ifndef EFSW_WATCHERINOTIFY_HPP
#define EFSW_WATCHERINOTIFY_HPP

#include <efsw/FileWatcherImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

namespace efsw {

class FileWatcherFSEvents;

class WatcherFSEvents : public Watcher
{
	public:
		WatcherFSEvents();
		
		WatcherFSEvents( WatchID id, std::string directory, FileWatchListener * listener, bool recursive, WatcherFSEvents * parent = NULL );
		
		~WatcherFSEvents();

		bool inParentTree( WatcherFSEvents * parent );
		
		void init();

		void initAsync();
		
		void handleAction( const std::string& path, const Uint32& flags );
		
		WatcherFSEvents * Parent;
		
		FileWatcherFSEvents * FWatcher;
	
		FSEventStreamRef FSStream;
};

}

#endif

#endif
