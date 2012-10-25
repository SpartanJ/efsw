#include <efsw/FileWatcherGeneric.hpp>
#include <efsw/System.hpp>
#include <efsw/FileSystem.hpp>

namespace efsw
{

class WatchStruct
{
	public:
		WatchID					ID;
		FileWatchListener	*	Listener;
		DirectoryWatch *		DirWatch;
		FileWatcherImpl *		WatcherImpl;
		DirectoryWatch *		CurDirWatch;
		std::string				Directory;

		WatchStruct( WatchID id, const std::string& directory, FileWatchListener * fwl, FileWatcherImpl * fw, bool recursive ) :
			ID( id ),
			Listener( fwl ),
			WatcherImpl( fw ),
			Directory( directory )
		{
			FileSystem::dirAddSlashAtEnd( Directory );

			DirWatch = CurDirWatch = new DirectoryWatch( this, directory, recursive );
		}

		~WatchStruct()
		{
			efSAFE_DELETE( DirWatch );
		}

		void watch()
		{
			DirWatch->watch();
		}

};

DirectoryWatch::DirectoryWatch( WatchStruct * ws, const std::string& directory, bool recursive ) :
	Watcher( ws ),
	Directory( directory ),
	Recursive( recursive ),
	Deleted( false )
{
	/// Is this a recursive watch?
	if ( ws->Directory != directory )
	{
		/// Get the real directory
		Directory = ws->CurDirWatch->Directory + directory;
	}

	ws->CurDirWatch = this;

	FileSystem::dirAddSlashAtEnd( Directory );

	GetFiles( Directory, Files );

	if ( Recursive )
	{
		/// Create the subdirectories watchers
		FileInfoMap::iterator it = Files.begin();

		for ( ; it != Files.end(); it++ )
		{
			if ( it->second.isDirectory() )
			{
				Directories[ it->first ] = new DirectoryWatch( ws, it->first, recursive );

				ws->CurDirWatch = this;
			}
		}
	}
}

DirectoryWatch::~DirectoryWatch()
{
	/// If the directory was deleted mark the files as deleted
	if ( Deleted )
	{
		DirectoryWatch * oldWatch = Watcher->CurDirWatch;

		Watcher->CurDirWatch = this;

		FileInfoMap::iterator it;
		FileInfo fi;

		for ( it = Files.begin(); it != Files.end(); it++ )
		{
			fi = it->second;

			if ( !fi.isDirectory() )
			{
				Watcher->WatcherImpl->handleAction( Watcher, it->first, Actions::Delete );
			}
		}

		Watcher->CurDirWatch = oldWatch;
	}

	DirWatchMap::iterator it = Directories.begin();

	for ( ; it != Directories.end(); it++ )
	{
		if ( Deleted )
		{
			/// If the directory was deleted, mark the flag for file deletion
			it->second->Deleted = true;
		}

		efSAFE_DELETE( it->second );
	}
}

void DirectoryWatch::watch()
{
	DirectoryWatch * oldWatch = Watcher->CurDirWatch;

	FileInfoMap files;

	GetFiles( Directory, files );

	if ( files.empty() && Files.empty() )
	{
		return;
	}

	FileInfoMap FilesCpy = Files;
	FileInfoMap::iterator it;
	FileInfoMap::iterator fif;
	DirWatchMap::iterator dit;
	DirectoryWatch * dw;
	FileInfo fi;

	for ( it = files.begin(); it != files.end(); it++ )
	{
		fi	= it->second;

		/// File existed before?
		fif = Files.find( it->first );

		if ( fif != Files.end() )
		{
			/// Erase from the file list copy
			FilesCpy.erase( it->first );

			/// File changed?
			if ( (*fif).second != fi )
			{
				/// Update the new file info
				Files[ it->first ] = fi;

				/// handle modified event
				Watcher->CurDirWatch = this;
				Watcher->WatcherImpl->handleAction( Watcher, it->first, Actions::Modified );
			}
		}
		else
		{
			/// New file found
			Files[ it->first ] = fi;

			/// handle add event
			Watcher->CurDirWatch = this;
			Watcher->WatcherImpl->handleAction( Watcher, it->first, Actions::Add );
		}

		/// Is directory and recursive?
		if ( fi.isDirectory() && Recursive )
		{
			/// Directory existed?
			dit = Directories.find( it->first );

			if ( dit != Directories.end() )
			{
				dw = (*dit).second;

				/// Just watch
				dw->watch();
			}
			else
			{
				/// Creates the new directory watcher of the subfolder and check for new files
				dw = new DirectoryWatch( Watcher, it->first, Recursive );
				dw->watch();

				/// Add it to the list of directories
				Directories[ it->first ] = dw;
			}
		}
	}

	Watcher->CurDirWatch = oldWatch;

	/// The files or directories that remains were deleted
	for ( it = FilesCpy.begin(); it != FilesCpy.end(); it++ )
	{
		fi = it->second;

		if ( fi.isDirectory() )
		{
			/// Folder deleted

			/// Search the folder, it should exists
			dit = Directories.find( it->first );

			if ( dit != Directories.end() )
			{
				dw = (*dit).second;;

				/// Flag it as deleted so it fire the event for every file inside deleted
				dw->Deleted = true;

				/// Delete the DirectoryWatcher
				efSAFE_DELETE( dw );

				/// Remove the directory from the map
				Directories.erase( (*dit).first );
			}

		}

		/// File or Directory deleted
		Watcher->CurDirWatch = this;

		/// handle delete event
		Watcher->WatcherImpl->handleAction( Watcher, it->first, Actions::Delete );

		/// Remove the file or directory from the list of files
		Files.erase( it->first );
	}

	Watcher->CurDirWatch = oldWatch;
}

void DirectoryWatch::GetFiles( const std::string& directory, FileInfoMap& files )
{
	files = FileSystem::filesInfoFromPath( directory );
}

FileWatcherGeneric::FileWatcherGeneric() :
	mThread( NULL ),
	mLastWatchID( 0 )
{
	mInitOK = true;
}

FileWatcherGeneric::~FileWatcherGeneric()
{
	mInitOK = false;

	mThread->wait();

	efSAFE_DELETE( mThread );

	/// Delete the watches
	WatchList::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); it++ )
	{
		efSAFE_DELETE( (*it) );
	}
}

WatchID FileWatcherGeneric::addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive)
{
	std::string dir( directory );

	FileSystem::dirAddSlashAtEnd( dir );

	if ( !FileSystem::isDirectory( dir ) )
	{
		return Errors::Log::createLastError( Errors::FileNotFound, directory );
	}

	mLastWatchID++;

	WatchStruct * pWatch		= new WatchStruct( mLastWatchID, dir, watcher, this, recursive );

	mWatchesLock.lock();
	mWatches.push_back(pWatch);
	mWatchesLock.unlock();

	return pWatch->ID;
}

void FileWatcherGeneric::removeWatch( const std::string& directory )
{
	WatchList::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); it++ )
	{
		if ( (*it)->Directory == directory )
		{
			mWatchesLock.lock();
			mWatches.erase( it );
			mWatchesLock.unlock();
			return;
		}
	}
}

void FileWatcherGeneric::removeWatch(WatchID watchid)
{
	WatchList::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); it++ )
	{
		if ( (*it)->ID == watchid )
		{
			mWatchesLock.lock();
			mWatches.erase( it );
			mWatchesLock.unlock();
			return;
		}
	}
}

void FileWatcherGeneric::watch()
{
	if ( NULL == mThread )
	{
		mThread = new Thread( &FileWatcherGeneric::run, this );
		mThread->launch();
	}
}

void FileWatcherGeneric::run()
{
	do
	{
		mWatchesLock.lock();

		WatchList::iterator it = mWatches.begin();

		for ( ; it != mWatches.end(); it++ )
		{
			(*it)->watch();
		}

		mWatchesLock.unlock();

		if ( mInitOK ) System::sleep( 1000 );
	} while ( mInitOK );
}

void FileWatcherGeneric::handleAction(WatchStruct* watch, const std::string& filename, unsigned long action)
{
	watch->Listener->handleFileAction( watch->ID, watch->CurDirWatch->Directory, filename, (Action)action );
}

std::list<std::string> FileWatcherGeneric::directories()
{
	std::list<std::string> dirs;

	mWatchesLock.lock();

	WatchList::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); it++ )
	{
		dirs.push_back( (*it)->Directory );
	}

	mWatchesLock.unlock();

	return dirs;
}

}
