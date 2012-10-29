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
	FileWatcherKqueue::FileWatcherKqueue() :
		mThread( NULL )
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
	}

	WatchID FileWatcherKqueue::addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive)
	{
		if( !FileSystem::isDirectory( directory ) )
		{
			return Errors::Log::createLastError( Errors::FileNotFound, directory );
		}

		WatcherKqueue* watch = new WatcherKqueue(  ++mLastWatchID, directory, watcher, recursive, this );

		mWatchesLock.lock();
		mWatches.insert(std::make_pair(mLastWatchID, watch));
		mWatchesLock.unlock();

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

			System::sleep( 250 );
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
}

#endif
