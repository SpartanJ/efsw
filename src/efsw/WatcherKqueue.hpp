#ifndef _FW_WATCHEROSX_H_
#define _FW_WATCHEROSX_H_
#pragma once

#include <efsw/FileWatcherImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE

#include <map>
#include <sys/event.h>
#include <sys/types.h>

namespace efsw
{
	class FileWatcherKqueue;
	class WatcherKqueue;

	/// type for a map from WatchID to WatcherKqueue pointer
	typedef std::map<WatchID, WatcherKqueue*> WatchMap;

	typedef struct kevent KEvent;

	#define MAX_CHANGE_EVENT_SIZE 50

	class WatcherKqueue : public Watcher
	{
		public:
			WatcherKqueue( WatchID watchid, const std::string& dirname, FileWatchListener* listener, bool recursive, FileWatcherKqueue * watcher );

			~WatcherKqueue();

			void addFile(  const std::string& name, bool emitEvents = true );

			void removeFile( const std::string& name, bool emitEvents = true );

			// called when the directory is actually changed
			// means a file has been added or removed
			// rescans the watched directory adding/removing files and sending notices
			void rescan();

			void handleAction(const std::string& filename, efsw::Action action);

			void addAll();

			void removeAll();

			WatchID watchingDirectory( std::string dir );

			void watch();

			WatchID addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive);

			void removeWatch (WatchID watchid );
		protected:
			WatchMap			mWatches;
			int					mLastWatchID;

			// index 0 is always the directory
			KEvent				mChangeList[MAX_CHANGE_EVENT_SIZE];
			size_t				mChangeListCount;

			/// The descriptor for the kqueue
			int					mDescriptor;

			FileWatcherKqueue *	mWatcher;

			WatcherKqueue *		mParent;

	};
}

#endif

#endif
