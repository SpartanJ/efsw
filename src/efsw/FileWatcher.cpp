#include <efsw/efsw.hpp>
#include <efsw/FileWatcherImpl.hpp>
#include <efsw/FileWatcherGeneric.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32
#	include <efsw/FileWatcherWin32.hpp>
#	define FILEWATCHER_IMPL FileWatcherWin32
#elif EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE
#	include <efsw/FileWatcherKqueue.hpp>
#	define FILEWATCHER_IMPL FileWatcherKqueue
#elif EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY
#	include <efsw/FileWatcherInotify.hpp>
#	define FILEWATCHER_IMPL FileWatcherInotify
#else
#	define FILEWATCHER_IMPL FileWatcherGeneric
#endif

namespace efsw {

FileWatcher::FileWatcher() :
	mOutOfScopeLinks(false)
{
	mImpl = new FILEWATCHER_IMPL( this );

	if ( !mImpl->initOK() )
	{
		efSAFE_DELETE( mImpl );

		mImpl = new FileWatcherGeneric( this );
	}
}

FileWatcher::FileWatcher( bool useGenericFileWatcher ) :
	mOutOfScopeLinks(false)
{
	if ( useGenericFileWatcher )
	{
		mImpl = new FileWatcherGeneric( this );
	}
	else
	{
		mImpl = new FILEWATCHER_IMPL( this );

		if ( !mImpl->initOK() )
		{
			efSAFE_DELETE( mImpl );

			mImpl = new FileWatcherGeneric( this );
		}
	}
}

FileWatcher::~FileWatcher()
{
	efSAFE_DELETE( mImpl );
}

WatchID FileWatcher::addWatch(const std::string& directory, FileWatchListener* watcher)
{
	return mImpl->addWatch(directory, watcher, false);
}

WatchID FileWatcher::addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive)
{
	return mImpl->addWatch(directory, watcher, recursive);
}

void FileWatcher::removeWatch(const std::string& directory)
{
	mImpl->removeWatch(directory);
}

void FileWatcher::removeWatch(WatchID watchid)
{
	mImpl->removeWatch(watchid);
}

void FileWatcher::watch()
{
	mImpl->watch();
}

void FileWatcher::allowOutOfScopeLinks( bool allow )
{
	mOutOfScopeLinks = allow;
}

const bool& FileWatcher::allowOutOfScopeLinks() const
{
	return mOutOfScopeLinks;
}

}
