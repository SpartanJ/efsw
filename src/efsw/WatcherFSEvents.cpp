#include <efsw/WatcherFSEvents.hpp>
#include <efsw/FileWatcherFSEvents.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/Debug.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

namespace efsw {

WatcherFSEvents::WatcherFSEvents() :
	Watcher(),
	FWatcher( NULL ),
	FSStream( NULL ),
	WatcherGen( NULL ),
	initializedAsync( false ),
	mLastId(0)
{
}

WatcherFSEvents::WatcherFSEvents( WatchID id, std::string directory, FileWatchListener * listener, bool recursive, WatcherFSEvents * parent ) :
	Watcher( id, directory, listener, recursive ),
	FWatcher( NULL ),
	FSStream( NULL ),
	WatcherGen( NULL ),
	initializedAsync( false ),
	mLastId(0)
{
}

WatcherFSEvents::~WatcherFSEvents()
{
	if ( initializedAsync )
	{
		FSEventStreamStop( FSStream );
		FSEventStreamInvalidate( FSStream );
	}

	if ( NULL != FSStream )
	{
		FSEventStreamRelease( FSStream );
	}

	efSAFE_DELETE( WatcherGen );
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
	else
	{
		WatcherGen = new WatcherGeneric( ID, Directory, Listener, FWatcher, Recursive );
	}
	
	FSEventStreamContext ctx;
	/* Initialize context */
	ctx.version = 0;
	ctx.info = this;
	ctx.retain = NULL;
	ctx.release = NULL;
	ctx.copyDescription = NULL;

	FSStream = FSEventStreamCreate( kCFAllocatorDefault, &FileWatcherFSEvents::FSEventCallback, &ctx, CFDirectoryArray, kFSEventStreamEventIdSinceNow, 0.25, streamFlags );

	FWatcher->mNeedInit.push_back( this );

	CFRelease( CFDirectoryArray );
	CFRelease( CFDirectory );
}

void WatcherFSEvents::initAsync()
{
	FSEventStreamScheduleWithRunLoop( FSStream, FWatcher->mRunLoopRef, kCFRunLoopDefaultMode );
	FSEventStreamStart( FSStream );
	initializedAsync = true;
}

void WatcherFSEvents::sendFileAction( WatchID watchid, const std::string& dir, const std::string& filename, Action action, std::string oldFilename )
{
	Listener->handleFileAction( watchid, FileSystem::precomposeFileName( dir ), FileSystem::precomposeFileName( filename ), action, oldFilename );
}

void WatcherFSEvents::handleAddModDel( const Uint32& flags, const std::string& path, std::string& dirPath, std::string& filePath )
{
	if ( flags & efswFSEventStreamEventFlagItemCreated )
	{
		if ( FileInfo::exists( path ) )
		{
			sendFileAction( ID, dirPath, filePath, Actions::Add );
		}
	}

	if ( flags & efswFSEventsModified )
	{
		sendFileAction( ID, dirPath, filePath, Actions::Modified );
	}

	if ( flags & efswFSEventStreamEventFlagItemRemoved )
	{
		// Since i don't know the order, at least i try to keep the data consistent with the real state
		if ( !FileInfo::exists( path ) )
		{
			sendFileAction( ID, dirPath, filePath, Actions::Delete );
		}
	}
}

void WatcherFSEvents::CheckLostEvents()
{
	if ( !mLastsRenamed.empty() )
	{
		for ( std::list<FileInfo>::iterator it = mLastsRenamed.begin(); it != mLastsRenamed.end(); it++ )
		{
			FileInfo tInfo = *it;

			if ( !tInfo.Filepath.empty() )
			{
				// Some file must have been moved, not renamed
				efDEBUG( "Weird crap %s\n", tInfo.Filepath.c_str() );

				if ( tInfo.exists() )
					sendFileAction( ID, FileSystem::pathRemoveFileName( tInfo.Filepath ), FileSystem::fileNameFromPath( tInfo.Filepath ), Actions::Add );
				else
					sendFileAction( ID, FileSystem::pathRemoveFileName( tInfo.Filepath ), FileSystem::fileNameFromPath( tInfo.Filepath ), Actions::Delete );
			}
		}

		mLastsRenamed.clear();
	}
}

void WatcherFSEvents::handleAction( const std::string& path, const Uint32& flags, const Uint64& eventId )
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
		if ( flags & ( efswFSEventStreamEventFlagItemCreated |
					   efswFSEventStreamEventFlagItemRemoved |
					   efswFSEventStreamEventFlagItemRenamed )
		)
		{
			std::string dpath( FileSystem::pathRemoveFileName( path ) );

			if ( dpath != Directory )
			{
				DirsChanged.insert( dpath );
			}
		}

		// This is a mess. But it's FSEvents faults, because shrinks events from the same file in one single event ( so there's no order for them )
		// For example a file could have been added modified and erased, but i can't know if first was erased and then added and modified, or added, then modified and then erased.
		// I don't know what they were thinking by doing this...
		efDEBUG( "Event in: %s - flags: %ld\n", path.c_str(), flags );

		if ( !( flags & efswFSEventStreamEventFlagItemRenamed ) )
		{
			handleAddModDel( flags, path, dirPath, filePath );
		}
		else
		{
			if ( eventId - mLastId != 1 )
			{
				CheckLostEvents();

				efDEBUG( "New Last Renamed: %s\n", filePath.c_str() );

				if ( !( flags & efswFSEventsModified ) ) {
					mLastsRenamed.push_back( FileInfo( path ) );
				}

				handleAddModDel( flags, path, dirPath, filePath );
			}
			else
			{
				FileInfo tFile = mLastsRenamed.back();
				std::string oldDir( FileSystem::pathRemoveFileName( tFile.Filepath ) );
				std::string newDir( FileSystem::pathRemoveFileName( path ) );
				std::string oldFilepath( FileSystem::fileNameFromPath( tFile.Filepath ) );

				if ( tFile.Filepath != path )
				{
					if ( oldDir == newDir )
					{
						if ( !tFile.exists() )
						{
							sendFileAction( ID, dirPath, filePath, Actions::Moved, oldFilepath );
						}
						else
						{
							sendFileAction( ID, dirPath, oldFilepath, Actions::Moved, filePath );
						}
					}
					else
					{
						sendFileAction( ID, oldDir, oldFilepath, Actions::Delete );
						sendFileAction( ID, dirPath, filePath, Actions::Add );

						if ( flags & efswFSEventsModified )
						{
							sendFileAction( ID, dirPath, filePath, Actions::Modified );
						}
					}
				}
				else
				{
					handleAddModDel( flags, path, dirPath, filePath );
				}

				mLastsRenamed.pop_back();
			}

			mLastId = eventId;
		}
	}
	else
	{
		efDEBUG( "Directory: %s changed\n", path.c_str() );
		DirsChanged.insert( path );
	}
}

void WatcherFSEvents::process()
{
	std::set<std::string>::iterator it = DirsChanged.begin();

	for ( ; it != DirsChanged.end(); it++ )
	{
		if ( !FileWatcherFSEvents::isGranular() )
		{
			WatcherGen->watchDir( (*it) );
		}
		else
		{
			sendFileAction( ID, FileSystem::pathRemoveFileName( (*it) ), FileSystem::fileNameFromPath( (*it) ), Actions::Modified );
		}
	}

	DirsChanged.clear();

	CheckLostEvents();
}

} 

#endif
