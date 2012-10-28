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
	int comparator(const void* ke1, const void* ke2)
	{
		const KEvent * kev1	= reinterpret_cast<const KEvent*>( ke1 );
		const KEvent * kev2	= reinterpret_cast<const KEvent*>( ke2 );

		if ( NULL != kev2->udata )
		{
			FileInfo * fi1		= reinterpret_cast<FileInfo*>( kev1->udata );
			FileInfo * fi2		= reinterpret_cast<FileInfo*>( kev2->udata );

			return strcmp( fi1->Filepath.c_str(), fi2->Filepath.c_str() );
		}

		return 1;
	}
	
	WatcherKqueue::WatcherKqueue(WatchID watchid, const std::string& dirname, FileWatchListener* listener, bool recursive ) :
		Watcher( watchid, dirname, listener, recursive )
	{
		mDescriptor			= kqueue();

		memset( mChangeList, 0, MAX_CHANGE_EVENT_SIZE );

		mChangeListCount = 0;

		addAll();
	}

	WatcherKqueue::~WatcherKqueue()
	{
		close( mDescriptor );
	}

	void WatcherKqueue::addFile(const std::string& name, bool imitEvents)
	{
		fprintf(stderr, "ADDED: %s\n", name.c_str());

		int fd = open(name.c_str(), O_RDONLY);

		if(fd == -1)
		{
			Errors::Log::createLastError( Errors::FileNotFound, name );
			return;
		}

		++mChangeListCount;

		// create entry
		FileInfo * entry = new FileInfo( name );

		// set the event data at the end of the list
		EV_SET(
				&mChangeList[mChangeListCount],
				fd,
				EVFILT_VNODE,
				EV_ADD | EV_ENABLE | EV_ONESHOT,
				NOTE_DELETE | NOTE_EXTEND | NOTE_WRITE | NOTE_ATTRIB,
				0,
				(void*)entry
		);

		// qsort
		qsort(mChangeList + 1, mChangeListCount, sizeof(KEvent), comparator);

		// handle action
		if( imitEvents )
		{
			handleAction(name, Actions::Add);
		}
	}

	void WatcherKqueue::removeFile( const std::string& name, bool imitEvents )
	{
		// bsearch
		KEvent target;

		FileInfo tempEntry( name );

		target.udata = &tempEntry;

		KEvent * ke = (KEvent*)bsearch(&target, &mChangeList, mChangeListCount + 1, sizeof(KEvent), comparator);

		if(!ke)
		{
			Errors::Log::createLastError( Errors::FileNotFound, name );
			return;
		}

		tempEntry.Filepath = "";

		// delete
		close(ke->ident);

		// handle action
		if( imitEvents )
		{
			handleAction( name, Actions::Delete );
		}

		FileInfo * del = reinterpret_cast<FileInfo*>( ke->udata );

		efSAFE_DELETE( del );

		memset(ke, 0, sizeof(KEvent));

		// move end to current
		memcpy(ke, &mChangeList[mChangeListCount], sizeof(KEvent));
		memset(&mChangeList[mChangeListCount], 0, sizeof(KEvent));
		--mChangeListCount;

		// qsort
		qsort(&mChangeList[1], mChangeListCount, sizeof(KEvent), comparator);
	}

	// called when the directory is actually changed
	// means a file has been added or removed
	// rescans the watched directory adding/removing files and sending notices
	void WatcherKqueue::rescan()
	{
		// if new file, call addFile
		// if missing file, call removeFile
		// if timestamp modified, call handleAction(filename, ACTION_MODIFIED);

		DIR* dir = opendir(Directory.c_str());
		if(!dir)
			return;

		struct dirent* dentry;
		KEvent* ke = &mChangeList[1];
		FileInfo * entry = NULL;

		//struct stat attrib;
		while( (dentry = readdir( dir ) ) != NULL)
		{
			std::string fname( Directory + FileSystem::getOSSlash() + std::string( dentry->d_name ) );

			FileInfo fi( fname );

			if ( !fi.isRegularFile() )
			{
				continue;
			}

			if( ke <= &mChangeList[ mChangeListCount ] )
			{
				entry = reinterpret_cast<FileInfo*>( ke->udata );

				int result = strcmp( entry->Filepath.c_str(), fname.c_str() );

				if(result == 0)
				{
					if ( *entry != fi )
					{
						*entry = fi;

						handleAction( entry->Filepath, Actions::Modified );
					}
				}
				else if(result < 0)
				{
					removeFile( entry->Filepath );
				}
				else
				{
					addFile(fname);
				}
			}
			else
			{
				// just add
				addFile(fname);
			}

			ke++;
		}

		closedir(dir);
	}

	void WatcherKqueue::handleAction(const std::string& filename, efsw::Action action)
	{
		Listener->handleFileAction( ID, Directory, FileSystem::fileNameFromPath( filename ), action );
	}

	void WatcherKqueue::addAll()
	{
		// scan directory and call addFile(name, false) on each file
		FileSystem::dirAddSlashAtEnd( Directory );

		// add base dir
		int fd = open( Directory.c_str(), O_RDONLY );

		EV_SET(
				&mChangeList[0],
				fd,
				EVFILT_VNODE,
				EV_ADD | EV_ENABLE | EV_ONESHOT,
				NOTE_DELETE | NOTE_EXTEND | NOTE_WRITE | NOTE_ATTRIB,
				0,
				0
		);

		fprintf(stderr, "ADDED: %s\n", Directory.c_str());

		FileInfoMap files = FileSystem::filesInfoFromPath( Directory );

		for ( FileInfoMap::iterator it = files.begin(); it != files.end(); it++ )
		{
			FileInfo& fi = it->second;

			if ( fi.isRegularFile() )
			{
				addFile( fi.Filepath , false );
			}
			else if ( Recursive && fi.isDirectory() )
			{
				// create another watcher
			}
		}
	}

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

		WatcherKqueue* watch = new WatcherKqueue(  ++mLastWatchID, directory, watcher, recursive );
		mWatchesLock.lock();
		mWatches.insert(std::make_pair(mLastWatchID, watch));
		mWatchesLock.unlock();
		return mLastWatchID;
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
			int nev = 0;
			KEvent event;

			WatchMap::iterator iter	= mWatches.begin();
			WatchMap::iterator end	= mWatches.end();

			for (; iter != end; ++iter)
			{
				WatcherKqueue* watch = iter->second;

				while( ( nev = kevent( watch->mDescriptor, (KEvent*)&(watch->mChangeList), watch->mChangeListCount + 1, &event, 1, &mTimeOut ) ) != 0 )
				{
					if( nev == -1 )
					{
						perror("kevent");
					}
					else
					{
						FileInfo * entry = NULL;

						if( ( entry = reinterpret_cast<FileInfo*> ( event.udata ) ) != NULL )
						{
							fprintf( stderr, "File: %s -- ", entry->Filepath.c_str() );

							if (event.fflags & NOTE_DELETE)
							{
								fprintf( stderr, "File deleted\n" );

								watch->removeFile( entry->Filepath );
							}
							else if (event.fflags & NOTE_EXTEND	||
								event.fflags & NOTE_WRITE	||
								event.fflags & NOTE_ATTRIB
							)
							{
								fprintf( stderr, "modified\n" );

								*entry = FileInfo( entry->Filepath );

								watch->handleAction( entry->Filepath, efsw::Actions::Modified );
							}
						}
						else
						{
							fprintf( stderr, "Dir: %s -- rescanning\n", watch->Directory.c_str() );

							watch->rescan();
						}
					}
				}

				System::sleep( 500 );
			}
		} while( mInitOK );
	}

	void FileWatcherKqueue::removeWatch(const std::string& directory)
	{
		mWatchesLock.lock();

		WatchMap::iterator iter = mWatches.begin();
		WatchMap::iterator end = mWatches.end();
		for(; iter != end; ++iter)
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
