#include <efsw/FileWatcherImpl.hpp>
#include <efsw/String.hpp>
#include <efsw/System.hpp>

namespace efsw {

Watcher::Watcher() :
	ID(0),
	Directory(""),
	Listener(NULL),
	Recursive(false)
{
}

Watcher::Watcher( WatchID id, std::string directory, FileWatchListener * listener, bool recursive ) :
	ID( id ),
	Directory( directory ),
	Listener( listener ),
	Recursive( recursive )
{
}

FileWatcherImpl::FileWatcherImpl( FileWatcher * parent ) :
	mFileWatcher( parent ),
	mInitOK( false )
{
	System::maxFD();
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
