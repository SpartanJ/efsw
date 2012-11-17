#include <efsw/WatcherFSEvents.hpp>
#include <efsw/FileWatcherFSEvents.hpp>
#include <efsw/FileSystem.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

namespace efsw {

WatcherFSEvents::WatcherFSEvents() :
	Watcher(),
	Parent( NULL ),
	FileWatcher( NULL )
{
}

WatcherFSEvents::WatcherFSEvents( WatchID id, std::string directory, FileWatchListener * listener, bool recursive, WatcherFSEvents * parent ) :
	Watcher( id, directory, listener, recursive ),
	Parent( parent ),
	FileWatcher( NULL )
{
}

WatcherFSEvents::~WatcherFSEvents()
{
	CFRelease( CFDirectoryArray );
	CFRelease( CFDirectory );
	
    FSEventStreamStop( FSStream );
    FSEventStreamInvalidate( FSStream );
    FSEventStreamRelease( FSStream );
}

bool WatcherFSEvents::inParentTree( WatcherFSEvents * parent )
{
	WatcherFSEvents * tNext = Parent;

	while ( NULL != tNext )
	{
		if ( tNext == parent )
		{
			return true;
		}

		tNext = tNext->Parent;
	}

	return false;
}

void WatcherFSEvents::init()
{
	CFDirectory			= CFStringCreateWithCString( NULL, Directory.c_str(), kCFStringEncodingUTF8 );
	CFDirectoryArray	= CFArrayCreate( NULL, (const void **)&CFDirectory, 1, NULL );
	
	Uint32 streamFlags = kFSEventStreamCreateFlagNone;
	
	if ( FileWatcherFSEvents::isGranular() )
	{
		streamFlags = efswFSEventStreamCreateFlagFileEvents;
	}
	
	FSStream = FSEventStreamCreate( NULL, &FileWatcherFSEvents::FSEventCallback, (void*)this, CFDirectoryArray, kFSEventStreamEventIdSinceNow, 0, streamFlags );
	
	FSEventStreamScheduleWithRunLoop( FSStream, FileWatcher->mRunLoopRef, kCFRunLoopDefaultMode );
	FSEventStreamStart( FSStream );
}

void WatcherFSEvents::handleAction( const std::string& path, const Uint32& flags )
{
	static std::string lastRenamed;
	
	if ( flags & (	kFSEventStreamEventFlagUserDropped |
					kFSEventStreamEventFlagKernelDropped |
					kFSEventStreamEventFlagEventIdsWrapped |
					kFSEventStreamEventFlagHistoryDone |
					kFSEventStreamEventFlagMount |
					kFSEventStreamEventFlagUnmount |
					kFSEventStreamEventFlagRootChanged))
	{
		return;
	}
    
    if ( !Recursive )
    {
		/** In case that is not recursive the watcher, ignore the events from subfolders */
		if ( path.find_last_of( FileSystem::getOSSlash() ) != Directory.size() - 1 )
		{
			return;
		}
	}
	
	std::string dirPath( FileSystem::pathRemoveFileName( path ) );
	std::string filePath( FileSystem::fileNameFromPath( path ) );
	
	if ( FileWatcherFSEvents::isGranular() )
	{
		if ( flags & efswFSEventStreamEventFlagItemCreated )
		{
			Listener->handleFileAction( ID, dirPath, filePath, Actions::Add );
		}
		else if ( flags & efswFSEventStreamEventFlagItemModified )
		{
			Listener->handleFileAction( ID, dirPath, filePath, Actions::Modified );
		}
		else if ( flags & efswFSEventStreamEventFlagItemRemoved )
		{
			Listener->handleFileAction( ID, dirPath, filePath, Actions::Delete );
		}
		else if ( flags & efswFSEventStreamEventFlagItemRenamed )
		{
			if ( lastRenamed.empty() )
			{
				lastRenamed = path;
			}
			else
			{
				Listener->handleFileAction( ID, dirPath, filePath, Actions::Moved, lastRenamed );
			}
		}
	}
	else
	{
		
	}
}

} 

#endif
