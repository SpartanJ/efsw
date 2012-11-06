#include <efsw/WatcherKqueue.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <efsw/FileSystem.hpp>
#include <efsw/System.hpp>
#include <efsw/FileWatcherKqueue.hpp>
#include <efsw/Debug.hpp>
#include <efsw/String.hpp>

namespace efsw {

WatcherKqueue::WatcherKqueue(WatchID watchid, const std::string& dirname, FileWatchListener* listener, bool recursive, FileWatcherKqueue * watcher, WatcherKqueue * parent ) :
	Watcher( watchid, dirname, listener, recursive ),
	mKqueue( kqueue() ),
	mDescriptor( -1 ),
	mLastWatchID(0),
	mWatcher( watcher ),
	mParent( parent )
{
	if ( -1 == mKqueue )
	{
		efDEBUG( "kqueue() returned invalid descriptor.\n" );
	}
	else
	{
		mWatcher->addFD();
	}
}

WatcherKqueue::~WatcherKqueue()
{
	// Remove the childs watchers ( sub-folders watches )
	removeAll();

	close( mDescriptor );

	mWatcher->removeFD();

	close( mKqueue );

	mWatcher->removeFD();
}

void WatcherKqueue::addAll()
{
	// scan directory and call addFile(name, false) on each file
	FileSystem::dirAddSlashAtEnd( Directory );

	efDEBUG( "addAll(): Added folder: %s\n", Directory.c_str());

	// add base dir
	mDescriptor = open( Directory.c_str(), O_RDONLY );

	if ( -1 == mDescriptor )
	{
		efDEBUG( "addAll(): Couldn't open folder: %s\n", Directory.c_str() );
	}
	else
	{
		mWatcher->addFD();
	}

	// Creates the kevent for the folder
	EV_SET(
		&mKevent,
		mDescriptor,
		EVFILT_VNODE,
		EV_ADD | EV_ENABLE | EV_ONESHOT,
		NOTE_DELETE | NOTE_EXTEND | NOTE_WRITE | NOTE_ATTRIB,
		0,
		0
	);

	// Get the files and directories from the directory
	FileInfoMap files = FileSystem::filesInfoFromPath( Directory );

	for ( FileInfoMap::iterator it = files.begin(); it != files.end(); it++ )
	{
		FileInfo& fi = it->second;

		if ( fi.isRegularFile() )
		{
			// Add the regular files kevent
			addFile( fi.Filepath , false );
		}
		else if ( Recursive && fi.isDirectory() )
		{
			// Create another watcher for the subfolders ( if recursive )
			WatchID id = addWatch( fi.Filepath, Listener, Recursive, this );

			// If the watcher is not adding the watcher means that the directory was created
			if ( id > 0 && !mWatcher->isAddingWatcher() )
			{
				handleFolderAction( fi.Filepath, Actions::Add );
			}
		}
	}
}

void WatcherKqueue::removeAll()
{
	efDEBUG( "removeAll(): Removing all child watchers\n" );

	std::list<WatchID> erase;

	for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); it++ )
	{
		efDEBUG( "removeAll(): Removed child watcher %s\n", it->second->Directory.c_str() );

		erase.push_back( it->second->ID );
	}

	for ( std::list<WatchID>::iterator eit = erase.begin(); eit != erase.end(); eit++ )
	{
		removeWatch( *eit );
	}
}

void WatcherKqueue::addFile(const std::string& name, bool emitEvents)
{
	// create entry
	mFiles[ name ] = FileInfo( name );

	// handle action
	if( emitEvents )
	{
		handleAction(name, Actions::Add);
	}
}

void WatcherKqueue::removeFile( const std::string& name, bool emitEvents )
{
	mFiles.erase( name );

	// handle action
	if ( emitEvents )
	{
		handleAction( name, Actions::Delete );
	}
}

// called when the directory is actually changed
// means a file has been added or removed
// rescans the watched directory adding/removing files and sending notices
void WatcherKqueue::rescan()
{
	efDEBUG( "rescan(): Rescanning: %s\n", Directory.c_str() );

	// if new file, call addFile
	// if missing file, call removeFile
	// if timestamp modified, call handleAction(filename, ACTION_MODIFIED);

	DIR* dir = opendir( Directory.c_str() );

	if( !dir )
	{
		// If the directory is a sub-folder of a user added watcher
		// remove it
		if ( NULL != mParent )
		{	// Flag for deletion
			efDEBUG( "rescan(): Directory %s flaged for deletion.\n", Directory.c_str() );
			mParent->mErased.push_back( ID );
		}

		return;
	}

	struct dirent * dentry;
	FileInfoMap FilesCpy = mFiles;
	FileInfo fi;
	FileInfoMap::iterator fif;

	// struct stat attrib
	// update the directory
	while( ( dentry = readdir( dir ) ) != NULL)
	{
		std::string dname( dentry->d_name );
		std::string fname( Directory + dname );

		if (  "." == dname || ".." == dname )
		{
			continue;
		}

		fi = FileInfo( fname );

		if ( Recursive && fi.isDirectory() )
		{
			WatchID id;

			FileSystem::dirAddSlashAtEnd( fi.Filepath );

			// Create another watcher if the watcher doesn't exists
			if ( ( id = watchingDirectory( fi.Filepath ) ) < 0 )
			{
				id = addWatch( fi.Filepath, Listener, Recursive, this );

				if ( id > 0 )
				{
					handleFolderAction( fi.Filepath, Actions::Add );

					efDEBUG( "rescan(): Directory %s added\n", fi.Filepath.c_str() );
				}
			}
			else
			{
				FilesCpy.erase( fi.Filepath );
			}
		}
		else if ( fi.isRegularFile() ) // Skip non-regular files
		{
			/// File existed before?
			fif = mFiles.find( fi.Filepath );

			if ( fif != mFiles.end() )
			{
				/// Erase from the file list copy
				FilesCpy.erase( fif->first );

				/// File changed?
				if ( fif->second != fi )
				{
					/// Update the new file info
					mFiles[ fi.Filepath ] = fi;

					/// handle modified event
					handleAction( fi.Filepath, Actions::Modified );
				}

				FilesCpy.erase( fi.Filepath );
			}
			else /// Only add regular files
			{
				/// New file found
				mFiles[ fi.Filepath ] = fi;

				/// handle add event
				handleAction( fi.Filepath, Actions::Add );
			}
		}
	}

	closedir(dir);

	/// The files or directories that remains were deleted
	for ( FileInfoMap::iterator it = FilesCpy.begin(); it != FilesCpy.end(); it++ )
	{
		fi = it->second;

		if ( fi.isDirectory() )
		{
			WatchID id = watchingDirectory( fi.Filepath );

			// Create another watcher if the watcher doesn't exists
			if ( id < 0 )
			{
				mErased.push_back( id );
			}
		}
		else if ( fi.isRegularFile() )
		{
			removeFile( fi.Filepath, true );
		}
	}
}

WatchID WatcherKqueue::watchingDirectory( std::string dir )
{
	for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); it++ )
	{
		if ( it->second->Directory == dir )
		{
			return it->first;
		}
	}

	return Errors::FileNotFound;
}

void WatcherKqueue::handleAction( const std::string& filename, efsw::Action action )
{
	Listener->handleFileAction( ID, Directory, FileSystem::fileNameFromPath( filename ), action );
}

void WatcherKqueue::handleFolderAction( std::string filename, efsw::Action action )
{
	FileSystem::dirRemoveSlashAtEnd( filename );

	handleAction( filename, action );
}

void WatcherKqueue::watch()
{
	int nev = 0;
	KEvent event;

	// First iterate the childs, to get the events from the deepest folder, to the watcher childs
	for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it )
	{
		it->second->watch();
	}

	// Then we get the the events of the current folder
	while( ( nev = kevent( mKqueue, &mKevent, 1, &event, 1, &mWatcher->mTimeOut ) ) != 0 )
	{
		// An error ocurred?
		if( nev == -1 )
		{
			efDEBUG( "watch(): Error on directory %s\n", Directory.c_str() );
			perror("kevent");
			break;
		}
		else
		{
			rescan();
		}
	}

	// Erase all watchers marked for deletion
	if ( !mErased.empty() )
	{
		efDEBUG( "watch(): Directory %s is erasing childs. %ld deletions pending.\n", Directory.c_str(), mErased.size() );

		for ( std::vector<WatchID>::iterator eit = mErased.begin(); eit != mErased.end(); eit++ )
		{
				if ( mWatches.find( (*eit) ) != mWatches.end() )
				{
					efDEBUG( "watch(): Directory %s removed. ID: %ld\n", mWatches[ (*eit) ]->Directory.c_str(), (*eit) );

					handleFolderAction( mWatches[ (*eit) ]->Directory, Actions::Delete );

					removeWatch( (*eit) );
				}
				else
				{
					efDEBUG( "watch(): Tryed to remove watcher ID: %ld, but it wasn't present.", (*eit) );
				}
		}

		mErased.clear();
	}
}

WatchID WatcherKqueue::addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive , WatcherKqueue *parent)
{
	std::string dir( directory );

	FileSystem::dirAddSlashAtEnd( dir );

	// This should never happen here
	if( !FileSystem::isDirectory( dir ) )
	{
		return Errors::Log::createLastError( Errors::FileNotFound, dir );
	}
	else if ( pathInWatches( dir ) || pathInParent( dir ) )
	{
		return Errors::Log::createLastError( Errors::FileRepeated, directory );
	}

	std::string curPath;
	std::string link( FileSystem::getLinkRealPath( dir, curPath ) );

	if ( "" != link )
	{
		if ( pathInWatches( link ) || pathInParent( link ) )
		{
			return Errors::Log::createLastError( Errors::FileRepeated, link );
		}
		else if ( !mWatcher->linkAllowed( curPath, link ) )
		{
			return Errors::Log::createLastError( Errors::FileOutOfScope, link );
		}
		else
		{
			dir = link;
		}
	}

	WatcherKqueue* watch = new WatcherKqueue( ++mLastWatchID, dir, watcher, recursive, mWatcher, parent );

	mWatches.insert(std::make_pair(mLastWatchID, watch));

	watch->addAll();

	return mLastWatchID;
}

void WatcherKqueue::removeWatch( WatchID watchid )
{
	WatchMap::iterator iter = mWatches.find(watchid);

	if(iter == mWatches.end())
		return;

	Watcher* watch = iter->second;

	mWatches.erase(iter);

	efSAFE_DELETE( watch );
}

bool WatcherKqueue::pathInWatches( const std::string& path )
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

bool WatcherKqueue::pathInParent( const std::string &path )
{
	WatcherKqueue * pNext = mParent;

	while ( NULL != pNext )
	{
		if ( pNext->pathInWatches( path ) )
		{
			return true;
		}

		pNext = pNext->mParent;
	}

	if ( mWatcher->pathInWatches( path ) )
	{
		return true;
	}

	if ( path == Directory )
	{
		return true;
	}

	return false;
}

}

#endif
