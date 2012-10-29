#include <efsw/WatcherKqueue.hpp>

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
#include <efsw/FileWatcherKqueue.hpp>

#define KEVENT_RESERVE_VALUE (10)

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
	
	WatcherKqueue::WatcherKqueue(WatchID watchid, const std::string& dirname, FileWatchListener* listener, bool recursive, FileWatcherKqueue * watcher ) :
		Watcher( watchid, dirname, listener, recursive ),
		mLastWatchID(0),
		mChangeListCount( 0 ),
		mDescriptor( kqueue() ),
		mWatcher( watcher ),
		mParent( NULL )
	{
		addAll();
	}

	WatcherKqueue::~WatcherKqueue()
	{
		// Remove the childs watchers ( sub-folders watches )
		removeAll();

		close( mDescriptor );
	}

	void WatcherKqueue::addAll()
	{
		// scan directory and call addFile(name, false) on each file
		FileSystem::dirAddSlashAtEnd( Directory );

		fprintf(stderr, "addAll(): Added folder: %s\n", Directory.c_str());

		// add base dir
		int fd = open( Directory.c_str(), O_RDONLY );

		mChangeList.resize( KEVENT_RESERVE_VALUE );

		// Creates the kevent for the folder
		EV_SET(
				&mChangeList[0],
				fd,
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
				WatchID id = addWatch( fi.Filepath, Listener, Recursive );

				// On success set the watch parent
				if ( id > 0 )
				{
					mWatches[ id ]->mParent = this;
				}
			}
		}
	}

	void WatcherKqueue::removeAll()
	{
		fprintf( stderr, "Removing all child watchers\n" );

		for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); it++ )
		{
			fprintf( stderr, "Removed child watcher %s\n", it->second->Directory.c_str() );

			removeWatch( it->second->ID );
		}
	}

	void WatcherKqueue::addFile(const std::string& name, bool emitEvents)
	{
		fprintf(stderr, "addFile(): Added: %s\n", name.c_str() );

		// Open the file to get the file descriptor
		int fd = open( name.c_str(), O_RDONLY );

		if( fd == -1 )
		{
			Errors::Log::createLastError( Errors::FileNotFound, name );
			return;
		}

		// increase the file kevent file count
		mChangeListCount++;

		if ( mChangeListCount + KEVENT_RESERVE_VALUE > mChangeList.size() &&
			 mChangeListCount % KEVENT_RESERVE_VALUE == 0 )
		{
			size_t reserve_size = mChangeList.size() + KEVENT_RESERVE_VALUE;
			mChangeList.resize( reserve_size );
			fprintf( stderr, "Reserverd more KEvents space for %s, space reserved %ld, list actual size %ld.\n", Directory.c_str(), reserve_size, mChangeListCount );
		}

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

		// qsort sort the list by name
		qsort(&mChangeList[1], mChangeListCount, sizeof(KEvent), comparator);

		// handle action
		if( emitEvents )
		{
			handleAction(name, Actions::Add);
		}
	}

	void WatcherKqueue::removeFile( const std::string& name, bool emitEvents )
	{
		fprintf(stderr, "removeFile(): Trying to remove file: %s\n", name.c_str() );

		// bsearch
		KEvent target;

		// Create a temporary file info to search the kevent ( searching the directory )
		FileInfo tempEntry( name );

		target.udata = &tempEntry;

		// Search the kevent
		KEvent * ke = (KEvent*)bsearch(&target, &mChangeList[0], mChangeListCount + 1, sizeof(KEvent), comparator);

		// Trying to remove a non-existing file?
		if( !ke )
		{
			Errors::Log::createLastError( Errors::FileNotFound, name );
			return;
		}

		/// @TODO: Why was this?
		tempEntry.Filepath = "";

		// handle action
		if ( emitEvents )
		{
			handleAction( name, Actions::Delete );
		}

		// Delete the user data ( FileInfo ) from the kevent closed
		FileInfo * del = reinterpret_cast<FileInfo*>( ke->udata );

		efSAFE_DELETE( del );

		// close the file descriptor from the kevent
		close( ke->ident );

		memset(ke, 0, sizeof(KEvent));

		// move end to current
		memcpy(ke, &mChangeList[mChangeListCount], sizeof(KEvent));
		memset(&mChangeList[mChangeListCount], 0, sizeof(KEvent));
		--mChangeListCount;

		// qsort list by name
		qsort(&mChangeList[1], mChangeListCount, sizeof(KEvent), comparator);
	}

	// called when the directory is actually changed
	// means a file has been added or removed
	// rescans the watched directory adding/removing files and sending notices
	void WatcherKqueue::rescan()
	{
		fprintf( stderr, "Rescanning: %s\n", Directory.c_str() );

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
				fprintf( stderr, "Directory %s flaged for deletion.\n", Directory.c_str() );
				mParent->mErased.push_back( ID );
			}

			return;
		}

		struct dirent * dentry;
		size_t kec			= 1;
		KEvent * ke			= &mChangeList[ kec ]; // first file kevent pointer
		FileInfo * entry	= NULL;
		//WatchMap watches	= mWatches;

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

			FileInfo fi( fname );

			if ( Recursive && fi.isDirectory() )
			{
				WatchID id;

				FileSystem::dirAddSlashAtEnd( fi.Filepath );

				// Create another watcher if the watcher doesn't exists
				if ( ( id = watchingDirectory( fi.Filepath ) ) < 0 )
				{
					id = addWatch( fi.Filepath, Listener, Recursive );

					if ( id > 0 )
					{
						mWatches[ id ]->mParent = this;

						fprintf( stderr, "Directory %s added\n", fi.Filepath.c_str() );
					}
				}
				else
				{
					// Remove from watches folders check list
					//watches.erase( id );
				}
			}
			else if ( fi.isRegularFile() ) // Skip non-regular files
			{
				if( ke <= &mChangeList[ mChangeListCount ] )
				{
					entry = reinterpret_cast<FileInfo*>( ke->udata );

					int result = strcmp( entry->Filepath.c_str(), fname.c_str() );

					// The file is still here?
					if( result == 0 )
					{
						// If the file changed, send the event and update the file info
						if ( *entry != fi )
						{
							*entry = fi;

							handleAction( entry->Filepath, Actions::Modified );
						}
					}
					else if( result < 0 )
					{	// If already passed the position that should be the entry, it means that it was deleted
						removeFile( entry->Filepath );
					}
					else
					{
						// The file seems to be before the kevent file, add it
						addFile(fname);
					}
				}
				else
				{
					// add the file at the end
					addFile(fname);
				}

				// Move to the next kvent
				kec++;
				ke = &mChangeList[kec];
			}
		}

		// remove the child watchers ( sub-folders of the directory ) that were erased
		// i think this is not needed, since every folder marks a flag for deletion after it last rescan
		/*for ( WatchMap::iterator it = watches.begin(); it != watches.end(); it++ )
		{
			fprintf( stderr, "Removing erased child watcher %s\n", it->second->Directory.c_str() );

			//removeWatch( it->second->ID );
			mErased.push_back( it->second->ID );
		}*/

		closedir(dir);
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
		while( ( nev = kevent( mDescriptor, &mChangeList[0], mChangeListCount + 1, &event, 1, &mWatcher->mTimeOut ) ) != 0 )
		{
			// An error ocurred?
			if( nev == -1 )
			{
				fprintf( stderr, "Error on directory %s\n", Directory.c_str() );
				perror("kevent");
				break;
			}
			else
			{
				FileInfo * entry = NULL;

				// If udate == NULL means that it is the fisrt element of the change list, the folder.
				// otherwise it is an event of some file inside the folder
				if( ( entry = reinterpret_cast<FileInfo*> ( event.udata ) ) != NULL )
				{
					fprintf( stderr, "File: %s -- ", entry->Filepath.c_str() );

					// If the event flag is delete... the file was deleted
					if ( event.fflags & NOTE_DELETE )
					{
						fprintf( stderr, "File deleted\n" );

						removeFile( entry->Filepath );
					}
					else if (	event.fflags & NOTE_EXTEND	||
								event.fflags & NOTE_WRITE	||
								event.fflags & NOTE_ATTRIB
					)
					{
						// The file was modified
						fprintf( stderr, "File modified\n" );

						*entry = FileInfo( entry->Filepath );

						handleAction( entry->Filepath, efsw::Actions::Modified );
					}
				}
				else
				{
					rescan();
				}
			}
		}

		// Erase all watchers marked for deletion
		if ( !mErased.empty() )
		{
			fprintf( stderr, "Directory %s is erasing childs. %ld deletions pending.\n", Directory.c_str(), mErased.size() );

			for ( std::vector<WatchID>::iterator eit = mErased.begin(); eit != mErased.end(); eit++ )
			{
				if ( mWatches.find( (*eit) ) != mWatches.end() )
				{
					fprintf( stderr, "Directory %s removed. ID: %ld\n", mWatches[ (*eit) ]->Directory.c_str(), (*eit) );

					removeWatch( (*eit) );
				}
				else
				{
					fprintf( stderr, "Tryed to remove watcher ID: %ld, but it wasn't present.", (*eit) );
				}
			}

			mErased.clear();
		}
	}

	WatchID WatcherKqueue::addWatch( const std::string& directory, FileWatchListener* watcher, bool recursive )
	{
		// This should never happen here
		if( !FileSystem::isDirectory( directory ) )
		{
			return Errors::Log::createLastError( Errors::FileNotFound, directory );
		}

		WatcherKqueue* watch = new WatcherKqueue( ++mLastWatchID, directory, watcher, recursive, mWatcher );

		mWatches.insert(std::make_pair(mLastWatchID, watch));

		return mLastWatchID;
	}

	void WatcherKqueue::removeWatch( WatchID watchid )
	{
		WatchMap::iterator iter = mWatches.find(watchid);

		if(iter == mWatches.end())
			return;

		WatcherKqueue* watch = iter->second;
		mWatches.erase(iter);

		efSAFE_DELETE( watch );
	}
}

#endif
