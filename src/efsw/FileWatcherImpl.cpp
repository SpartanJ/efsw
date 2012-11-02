#include <efsw/FileWatcherImpl.hpp>
#include <efsw/String.hpp>

#ifdef EFSW_PLATFORM_POSIX
#include <sys/resource.h>
#endif

namespace efsw {

static void maxFileDescriptors()
{
#ifdef EFSW_PLATFORM_POSIX
	static bool maxed = false;

	if ( !maxed )
	{
		struct rlimit limit;
		getrlimit( RLIMIT_NOFILE, &limit );
		limit.rlim_cur = limit.rlim_max;
		setrlimit( RLIMIT_NOFILE, &limit );

		maxed = true;
	}
#endif
}

Watcher::Watcher() :
	ID(0),
	Directory(""),
	Listener(NULL),
	Recursive(false)
{
	maxFileDescriptors();
}

Watcher::Watcher( WatchID id, std::string directory, FileWatchListener * listener, bool recursive ) :
	ID( id ),
	Directory( directory ),
	Listener( listener ),
	Recursive( recursive )
{
	maxFileDescriptors();
}

FileWatcherImpl::FileWatcherImpl( FileWatcher * parent ) :
	mFileWatcher( parent ),
	mInitOK( false )
{
}

FileWatcherImpl::~FileWatcherImpl()
{
}

bool FileWatcherImpl::initOK()
{
	return mInitOK;
}

bool FileWatcherImpl::linkAllowed( const std::string& curPath, const std::string& link )
{
	return mFileWatcher->allowOutOfScopeLinks() || -1 != String::strStartsWith( curPath, link );
}

}
