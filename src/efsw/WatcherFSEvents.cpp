#include <efsw/WatcherFSEvents.hpp>
#include <efsw/FileWatcherFSEvents.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/Debug.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

namespace efsw {

WatcherFSEvents::WatcherFSEvents() :
	Watcher(),
	Parent( NULL ),
	FWatcher( NULL ),
	LastWasRenamed( false )
{
}

WatcherFSEvents::WatcherFSEvents( WatchID id, std::string directory, FileWatchListener * listener, bool recursive, WatcherFSEvents * parent ) :
	Watcher( id, directory, listener, recursive ),
	Parent( parent ),
	FWatcher( NULL ),
	LastWasRenamed( false )
{
}

WatcherFSEvents::~WatcherFSEvents()
{
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
	CFStringRef CFDirectory			= CFStringCreateWithCString( NULL, Directory.c_str(), kCFStringEncodingUTF8 );
	CFArrayRef	CFDirectoryArray	= CFArrayCreate( NULL, (const void **)&CFDirectory, 1, NULL );
	
	Uint32 streamFlags = kFSEventStreamCreateFlagNone;
	
	if ( FileWatcherFSEvents::isGranular() )
	{
		streamFlags = efswFSEventStreamCreateFlagFileEvents;
	}
	
	FSEventStreamContext ctx;
	/* Initialize context */
	ctx.version = 0;
	ctx.info = this;
	ctx.retain = NULL;
	ctx.release = NULL;
	ctx.copyDescription = NULL;

	FSStream = FSEventStreamCreate( kCFAllocatorDefault, &FileWatcherFSEvents::FSEventCallback, &ctx, CFDirectoryArray, kFSEventStreamEventIdSinceNow, 0, streamFlags );

	FWatcher->mNeedInit.push_back( this );

	CFRelease( CFDirectoryArray );
	CFRelease( CFDirectory );
}

void WatcherFSEvents::initAsync()
{
	FSEventStreamScheduleWithRunLoop( FSStream, FWatcher->mRunLoopRef, kCFRunLoopDefaultMode );
	FSEventStreamStart( FSStream );
}

void WatcherFSEvents::handleAction( const std::string& path, const Uint32& flags )
{	
	if ( flags & (	kFSEventStreamEventFlagUserDropped |
					kFSEventStreamEventFlagKernelDropped |
					kFSEventStreamEventFlagEventIdsWrapped |
					kFSEventStreamEventFlagHistoryDone |
					kFSEventStreamEventFlagMount |
					kFSEventStreamEventFlagUnmount |
					kFSEventStreamEventFlagRootChanged) )
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
		if ( ( flags & kFSEventsModified ) != 0 && ( flags & kFSEventsRenamed ) == 0 )
		{
			Listener->handleFileAction( ID, dirPath, filePath, Actions::Modified );
			LastWasRenamed = false;
		}
		else if ( flags & efswFSEventStreamEventFlagItemRenamed )
		{
			efDEBUG( "Renamed event for %s\n", filePath.c_str() );

			if ( LastWasRenamed )
			{
				Listener->handleFileAction( ID, dirPath, filePath, Actions::Moved, LastRenamed );
				LastRenamed.clear();
				LastWasRenamed = false;
			}
			else
			{
				LastWasRenamed = true;
				LastRenamed = filePath;

				FileInfo fi( path );

				if ( fi.exists() & ( flags & efswFSEventStreamEventFlagItemRemoved ) )
				{
					LastWasRenamed = false;
					LastRenamed.clear();
				}
				else if ( !fi.exists() & ( flags & efswFSEventStreamEventFlagItemRemoved ) )
				{
					Listener->handleFileAction( ID, dirPath, filePath, Actions::Delete );
				}
			}
		}
		else if ( flags & efswFSEventStreamEventFlagItemCreated )
		{
			Listener->handleFileAction( ID, dirPath, filePath, Actions::Add );
			LastWasRenamed = false;
		}
		else if ( flags & efswFSEventStreamEventFlagItemRemoved )
		{
			Listener->handleFileAction( ID, dirPath, filePath, Actions::Delete );
			LastWasRenamed = false;
		}
		else
		{
			efDEBUG( "Event not filtered.\n" );
		}
	}
	else
	{
		
	}
}

} 

#endif
