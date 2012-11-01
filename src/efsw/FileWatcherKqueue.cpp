#include <efsw/FileWatcherKqueue.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE

#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <efsw/FileSystem.hpp>
#include <efsw/System.hpp>

namespace efsw
{

FileWatcherKqueue::FileWatcherKqueue( FileWatcher * parent ) :
	FileWatcherImpl( parent ),
	mThread( NULL ),
	mAddingWatcher( false ),
	mLastWatchID(0)
{
	mTimeOut.tv_sec		= 0;
	mTimeOut.tv_nsec	= 0;
	mInitOK				= true;
}

FileWatcherKqueue::~FileWatcherKqueue()
{
	WatchMap::iterator iter = mWatches.begin();

	for(; iter != mWatches.end(); ++iter)
	{
		efSAFE_DELETE( iter->second );
	}

	mWatches.clear();

	mInitOK = false;

	mThread->wait();

	efSAFE_DELETE( mThread );
}

WatchID FileWatcherKqueue::addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive)
{
	std::string dir( directory );

	FileSystem::dirAddSlashAtEnd( dir );

	if( !FileSystem::isDirectory( dir ) )
	{
		return Errors::Log::createLastError( Errors::FileNotFound, directory );
	}

	if ( pathInWatches( dir ) )
	{
		return Errors::Log::createLastError( Errors::FileRepeated, directory );
	}

	std::string curPath;
	std::string link( FileSystem::getLinkRealPath( dir, curPath ) );

	if ( "" != link )
	{
		if ( pathInWatches( link ) )
		{
			return Errors::Log::createLastError( Errors::FileRepeated, directory );
		}
		else if ( !linkAllowed( curPath, link ) )
		{
			return Errors::Log::createLastError( Errors::FileOutOfScope, dir );
		}
		else
		{
			dir = link;
		}
	}

	mAddingWatcher = true;

	/// @TODO: It seems that there is a limit of file descriptors opened
	/// for a process, so it should fallback to the generic watcher.
	WatcherKqueue* watch = new WatcherKqueue( ++mLastWatchID, dir, watcher, recursive, this );


	mWatchesLock.lock();
	mWatches.insert(std::make_pair(mLastWatchID, watch));
	mWatchesLock.unlock();

	watch->addAll();

	mAddingWatcher = false;

	return mLastWatchID;
}

void FileWatcherKqueue::removeWatch(const std::string& directory)
{
	mWatchesLock.lock();

	WatchMap::iterator iter = mWatches.begin();

	for(; iter != mWatches.end(); ++iter)
	{
		if(directory == iter->second->Directory)
		{
			removeWatch(iter->first);
			return;
		}
	}

	mWatchesLock.unlock();
}

void FileWatcherKqueue::removeWatch(WatchID watchid)
{
	mWatchesLock.lock();

	WatchMap::iterator iter = mWatches.find(watchid);

	if(iter == mWatches.end())
		return;

	WatcherKqueue* watch = iter->second;

	mWatches.erase(iter);

	efSAFE_DELETE( watch );

	mWatchesLock.unlock();
}

bool FileWatcherKqueue::isAddingWatcher() const
{
	return mAddingWatcher;
}

void FileWatcherKqueue::watch()
{
	if ( NULL == mThread )
	{
		mThread = new Thread( &FileWatcherKqueue::run, this );
		mThread->launch();
	}
}

void FileWatcherKqueue::run()
{
	do
	{
		mWatchesLock.lock();

		for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it )
		{
			it->second->watch();
		}

		mWatchesLock.unlock();

		System::sleep( 500 );
	} while( mInitOK );
}

void FileWatcherKqueue::handleAction(Watcher* watch, const std::string& filename, unsigned long action)
{
}

std::list<std::string> FileWatcherKqueue::directories()
{
	std::list<std::string> dirs;

	mWatchesLock.lock();

	WatchMap::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); it++ )
	{
		dirs.push_back( it->second->Directory );
	}

	mWatchesLock.unlock();

	return dirs;
}

bool FileWatcherKqueue::pathInWatches( const std::string& path )
{
	WatchMap::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); it++ )
	{
		if ( it->second->Directory == path )
		{
			return true;
		}
	}

	return false;
}

}

#endif
