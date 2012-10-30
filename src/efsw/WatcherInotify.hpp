#ifndef EFSW_WATCHERINOTIFY_HPP 
#define EFSW_WATCHERINOTIFY_HPP

#include <efsw/FileWatcherImpl.hpp>

namespace efsw {

class WatcherInotify : public Watcher
{
	public:
		WatcherInotify();
		
		WatcherInotify( WatchID id, std::string directory, FileWatchListener * listener, bool recursive, WatcherInotify * parent = NULL );

		WatcherInotify * Parent;

		bool inParentTree( WatcherInotify * parent );
};

}

#endif
