#include <efsw/WatcherKqueue.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE || EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <efsw/Debug.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/FileWatcherKqueue.hpp>
#include <efsw/String.hpp>
#include <efsw/System.hpp>
#include <efsw/WatcherGeneric.hpp>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define KEVENT_RESERVE_VALUE ( 10 )

#ifndef O_EVTONLY
#define O_EVTONLY ( O_RDONLY | O_NONBLOCK )
#endif

namespace efsw {

int comparator( const void* ke1, const void* ke2 ) {
	const KEvent* kev1 = reinterpret_cast<const KEvent*>( ke1 );
	const KEvent* kev2 = reinterpret_cast<const KEvent*>( ke2 );

	if ( NULL != kev2->udata ) {
		FileInfo* fi1 = reinterpret_cast<FileInfo*>( kev1->udata );
		FileInfo* fi2 = reinterpret_cast<FileInfo*>( kev2->udata );

		return strcmp( fi1->Filepath.c_str(), fi2->Filepath.c_str() );
	}

	return 1;
}

WatcherKqueue::WatcherKqueue( WatchID watchid, const std::string& dirname,
							  FileWatchListener* listener, bool recursive,
							  FileWatcherKqueue* watcher, WatcherKqueue* parent,
							  bool reportCrossDirectoryMoves ) :
	Watcher( watchid, dirname, listener, recursive ),
	mLastWatchID( 0 ),
	mChangeListCount( 0 ),
	mKqueue( kqueue() ),
	mWatcher( watcher ),
	mParent( parent ),
	mInitOK( true ),
	mErrno( 0 ),
	mReportCrossDirectoryMoves( reportCrossDirectoryMoves ),
	mCollectActions( false ),
	mNeedsRecursiveRescan( false ) {
	if ( -1 == mKqueue ) {
		efDEBUG(
			"kqueue() returned invalid descriptor for directory %s. File descriptors count: %ld\n",
			Directory.c_str(), mWatcher->mFileDescriptorCount.load() );

		mInitOK = false;
		mErrno = errno;
	} else {
		mWatcher->addFD();
	}
}

WatcherKqueue::~WatcherKqueue() {
	// Remove the children watchers ( sub-folders watches )
	removeAll();

	// The change list holds the directory at index 0, once addAll() opened it, and the files
	// at 1 to mChangeListCount. Each holds a descriptor, which closing the kqueue does not
	// close, and each file its FileInfo.
	if ( !mChangeList.empty() ) {
		for ( size_t i = 0; i <= mChangeListCount; i++ ) {
			FileInfo* fi = reinterpret_cast<FileInfo*>( mChangeList[i].udata );
			efSAFE_DELETE( fi );
			close( static_cast<int>( mChangeList[i].ident ) );
			mWatcher->removeFD();
		}
	}

	if ( -1 != mKqueue ) {
		close( mKqueue );
		mWatcher->removeFD();
	}
}

void WatcherKqueue::addAll() {
	if ( -1 == mKqueue ) {
		return;
	}

	// scan directory and call addFile(name, false) on each file
	FileSystem::dirAddSlashAtEnd( Directory );

	efDEBUG( "addAll(): Added folder: %s\n", Directory.c_str() );

	// add base dir
	int fd = open( Directory.c_str(), O_EVTONLY );

	if ( -1 == fd ) {
		efDEBUG( "addAll(): Couldn't open folder: %s\n", Directory.c_str() );

		if ( EACCES != errno ) {
			mInitOK = false;
		}

		mErrno = errno;

		return;
	}

	mDirSnap.setDirectoryInfo( Directory );
	mDirSnap.scan();

	mChangeList.resize( KEVENT_RESERVE_VALUE );

	// Creates the kevent for the folder
	EV_SET( &mChangeList[0], fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_ONESHOT,
			NOTE_DELETE | NOTE_EXTEND | NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME, 0, 0 );

	struct timespec timeout = { 0, 0 };
	kevent( mKqueue, mChangeList.data(), 1, NULL, 0, &timeout );

	mWatcher->addFD();

	// Get the files and directories from the directory
	FileInfoList files = FileSystem::filesInfoFromPath( Directory );

	for ( const auto& fi : files ) {
		if ( fi.isRegularFile() ) {
			// Add the regular files kevent
			addFile( fi.Filepath, false );
		} else if ( Recursive && fi.isDirectory() && fi.isReadable() ) {
			// Create another watcher for the subfolders ( if recursive ). A created directory
			// reports them with reportDirectories(), once every watch of its tree exists.
			addWatch( fi.Filepath, Listener, Recursive, this );
		}
	}
}

void WatcherKqueue::removeAll() {
	efDEBUG( "removeAll(): Removing all child watchers\n" );

	std::vector<WatchID> erase;

	for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); it++ ) {
		efDEBUG( "removeAll(): Removed child watcher %s\n", it->second->Directory.c_str() );

		erase.push_back( it->second->ID );
	}

	for ( std::vector<WatchID>::iterator eit = erase.begin(); eit != erase.end(); eit++ ) {
		removeWatch( *eit );
	}
}

bool WatcherKqueue::addFile( const std::string& name, bool emitEvents ) {
	efDEBUG( "addFile(): Added: %s\n", name.c_str() );

	// Open the file to get the file descriptor
	int fd = open( name.c_str(), O_EVTONLY );

	if ( fd == -1 ) {
		const int error = errno;
		efDEBUG( "addFile(): Could open file descriptor for %s. File descriptor count: %ld\n",
				 name.c_str(), mWatcher->mFileDescriptorCount.load() );

		Errors::Log::createLastError( Errors::FileNotReadable, name );

		// A file removed or renamed since the directory was listed is not a watcher failure.
		if ( EACCES != error && ENOENT != error ) {
			mInitOK = false;
		}

		mErrno = error;

		return false;
	}

	mWatcher->addFD();

	// increase the file kevent file count
	mChangeListCount++;

	if ( mChangeListCount + KEVENT_RESERVE_VALUE > mChangeList.size() &&
		 mChangeListCount % KEVENT_RESERVE_VALUE == 0 ) {
		size_t reserve_size = mChangeList.size() + KEVENT_RESERVE_VALUE;
		mChangeList.resize( reserve_size );
		efDEBUG( "addFile(): Reserverd more KEvents space for %s, space reserved %ld, list actual "
				 "size %ld.\n",
				 Directory.c_str(), reserve_size, mChangeListCount );
	}

	// create entry
	FileInfo* entry = new FileInfo( name );

	// set the event data at the end of the list
	EV_SET( &mChangeList[mChangeListCount], fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_ONESHOT,
			NOTE_DELETE | NOTE_EXTEND | NOTE_WRITE | NOTE_ATTRIB | NOTE_RENAME, 0, (void*)entry );

	struct timespec timeout = { 0, 0 };
	kevent( mKqueue, &mChangeList[mChangeListCount], 1, NULL, 0, &timeout );

	// qsort sort the list by name
	qsort( &mChangeList[1], mChangeListCount, sizeof( KEvent ), comparator );

	// handle action
	if ( emitEvents ) {
		handleAction( name, Actions::Add, "", *entry );
	}
	return true;
}

bool WatcherKqueue::removeFile( const std::string& name, bool emitEvents ) {
	efDEBUG( "removeFile(): Trying to remove file: %s\n", name.c_str() );

	// bsearch
	KEvent target;

	// Create a temporary file info to search the kevent ( searching the directory )
	FileInfo tempEntry( name );

	target.udata = &tempEntry;

	// Search the kevent
	KEvent* ke = (KEvent*)bsearch( &target, &mChangeList[1], mChangeListCount, sizeof( KEvent ),
								   comparator );

	// Trying to remove a non-existing file?
	if ( !ke ) {
		Errors::Log::createLastError( Errors::FileNotFound, name );
		efDEBUG( "File not removed\n" );
		return false;
	}

	efDEBUG( "File removed\n" );

	// Delete the user data ( FileInfo ) from the kevent closed
	FileInfo* del = reinterpret_cast<FileInfo*>( ke->udata );

	// handle action
	if ( emitEvents ) {
		handleAction( name, Actions::Delete, "", *del );
	}

	efSAFE_DELETE( del );

	// close the file descriptor from the kevent
	close( ke->ident );

	mWatcher->removeFD();

	memset( ke, 0, sizeof( KEvent ) );

	// move end to current
	memcpy( ke, &mChangeList[mChangeListCount], sizeof( KEvent ) );
	memset( &mChangeList[mChangeListCount], 0, sizeof( KEvent ) );
	--mChangeListCount;
	return true;
}

void WatcherKqueue::rescan() {
	efDEBUG( "rescan(): Rescanning: %s\n", Directory.c_str() );

	if ( !FileSystem::isDirectory( Directory ) ) {
		return;
	}

	DirectorySnapshotDiff Diff = mDirSnap.scan();

	if ( Diff.DirChanged ) {
		sendDirChanged();
	}

	if ( Diff.changed() ) {
		FileInfoList::iterator it;
		MovedList::iterator mit;

		/// Files
		// A file listed by the scan but gone before it could be opened, or never opened, is
		// not tracked: it is still reported from the snapshot, so that every reported file
		// gets its Delete.
		DiffIterator( FilesCreated ) {
			if ( !addFile( ( *it ).Filepath ) )
				handleAction( ( *it ).Filepath, Actions::Add, "", *it );
		}

		DiffIterator( FilesModified ) {
			handleAction( ( *it ).Filepath, Actions::Modified, "", *it );
		}

		DiffIterator( FilesDeleted ) {
			if ( !removeFile( ( *it ).Filepath ) )
				handleAction( ( *it ).Filepath, Actions::Delete, "", *it );
		}

		DiffMovedIterator( FilesMoved ) {
			handleAction( ( *mit ).second.Filepath, Actions::Moved, ( *mit ).first,
						  ( *mit ).second );
			removeFile( Directory + ( *mit ).first, false );
			addFile( ( *mit ).second.Filepath, false );
		}

		/// Directories
		DiffIterator( DirsCreated ) {
			// A non-recursive watch reports the directory as one of its entries, but does not
			// watch inside it, as addAll() does.
			if ( !Recursive ) {
				handleFolderAction( ( *it ).Filepath, Actions::Add, "", *it );
				continue;
			}
			// Reported once its watch exists, so that an entry created inside it by a listener
			// reacting to the event is not missed, but before the directories found inside it.
			WatchID id = addWatch( ( *it ).Filepath, Listener, Recursive, this );
			handleFolderAction( ( *it ).Filepath, Actions::Add, "", *it );
			auto added = id > 0 ? mWatches.find( id ) : mWatches.end();
			WatcherKqueue* child =
				added != mWatches.end() ? dynamic_cast<WatcherKqueue*>( added->second ) : NULL;
			if ( child )
				child->reportDirectories();
		}

		DiffIterator( DirsModified ) {
			handleFolderAction( ( *it ).Filepath, Actions::Modified, "", *it );
		}

		DiffIterator( DirsDeleted ) {
			handleFolderAction( ( *it ).Filepath, Actions::Delete, "", *it );

			Watcher* watch = findWatcher( ( *it ).Filepath );

			if ( NULL != watch ) {
				giveUpTree( watch, *it );
				removeWatch( watch->ID );
			}
		}

		DiffMovedIterator( DirsMoved ) {
			moveDirectory( Directory + ( *mit ).first, ( *mit ).second.Filepath, ( *mit ).second );
		}
	}
}

WatchID WatcherKqueue::watchingDirectory( std::string dir ) {
	Watcher* watch = findWatcher( dir );

	if ( NULL != watch ) {
		return watch->ID;
	}

	return Errors::FileNotFound;
}

void WatcherKqueue::retire() {
	Watcher::retire();
	mActionBatch.cancel();
	// Native descendants check the root, but generic ones, created when file descriptors run
	// out, report on their own: the whole tree is retired.
	for ( const auto& child : mWatches )
		child.second->retire();
}

void WatcherKqueue::handleAction( const std::string& filename, efsw::Action action,
								  const std::string& oldFilename, const FileInfo& fileInfo ) {
	WatcherKqueue* rootWatch = root();
	if ( rootWatch->Retired )
		return;
	if ( rootWatch->mCollectActions ) {
		FileActionBatch* shared = rootWatch->SharedActionBatch;
		FileActionBatch* batch = shared ? shared : &rootWatch->mActionBatch;
		batch->add( ID, Directory, FileSystem::fileNameFromPath( filename ), action,
					FileSystem::fileNameFromPath( oldFilename ), fileInfo,
					shared ? Listener : NULL, shared ? rootWatch : NULL );
	} else {
		Listener->handleFileAction( ID, Directory, FileSystem::fileNameFromPath( filename ), action,
									FileSystem::fileNameFromPath( oldFilename ) );
	}
}

void WatcherKqueue::handleFolderAction( std::string filename, efsw::Action action,
										const std::string& oldFilename, const FileInfo& fileInfo ) {
	FileSystem::dirRemoveSlashAtEnd( filename );

	handleAction( filename, action, oldFilename, fileInfo );
}

void WatcherKqueue::sendDirChanged() {
	// Reported by the parent like its other entries: collected while the root collects, so
	// that no listener runs in the middle of the root's scan.
	if ( NULL != mParent )
		mParent->handleFolderAction( Directory, Actions::Modified, "", mDirSnap.DirectoryInfo );
}

void WatcherKqueue::watch() {
	if ( -1 == mKqueue ) {
		return;
	}
	bool isRootWatch = NULL == mParent;
	if ( isRootWatch && ( SharedActionBatch || ( mReportCrossDirectoryMoves && Recursive ) ) ) {
		if ( !SharedActionBatch )
			mActionBatch.clear();
		mCollectActions = true;
		mNeedsRecursiveRescan = false;
	}

	int nev = 0;
	KEvent event;

	// First iterate the childs, to get the events from the deepest folder, to the watcher childs
	for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
		it->second->watch();
	}

	bool needScan = false;

	// Then we get the the events of the current folder
	while ( !mChangeList.empty() &&
			( nev = kevent( mKqueue, mChangeList.data(), mChangeListCount + 1, &event, 1,
							&mWatcher->mTimeOut ) ) != 0 ) {
		// An error ocurred?
		if ( nev == -1 ) {
			efDEBUG( "watch(): Error on directory %s\n", Directory.c_str() );
			perror( "kevent" );
			break;
		} else {
			FileInfo* entry = NULL;

			// If udate == NULL means that it is the first element of the change list, the folder.
			// otherwise it is an event of some file inside the folder
			if ( ( entry = reinterpret_cast<FileInfo*>( event.udata ) ) != NULL ) {
				efDEBUG( "watch(): File: %s ", entry->Filepath.c_str() );

				// If the event flag is delete... the file was deleted
				if ( event.fflags & NOTE_DELETE ) {
					efDEBUG( "deleted\n" );

					mDirSnap.removeFile( entry->Filepath );

					removeFile( entry->Filepath );
				} else if ( event.fflags & NOTE_EXTEND || event.fflags & NOTE_WRITE ||
							event.fflags & NOTE_ATTRIB ) {
					// The file was modified
					efDEBUG( "modified\n" );

					FileInfo fi( entry->Filepath );

					if ( fi != *entry ) {
						*entry = fi;

						mDirSnap.updateFile( entry->Filepath );

						handleAction( entry->Filepath, efsw::Actions::Modified, "", *entry );
					}
				} else if ( event.fflags & NOTE_RENAME ) {
					efDEBUG( "moved\n" );

					needScan = true;
				}
			} else {
				needScan = true;
			}
		}
	}

	if ( needScan ) {
		rescan();
		WatcherKqueue* rootWatch = root();
		if ( rootWatch->mCollectActions )
			rootWatch->mNeedsRecursiveRescan = true;
	}

	if ( isRootWatch && mCollectActions ) {
		if ( mNeedsRecursiveRescan )
			rescanTree();
		if ( !SharedActionBatch )
			mActionBatch.dispatch( Listener );
		mCollectActions = false;
	}
}

void WatcherKqueue::giveUpTree( Watcher* watch, const FileInfo& directory ) {
	WatcherKqueue* rootWatch = root();
	if ( !rootWatch->mCollectActions || NULL == rootWatch->SharedActionBatch )
		return;
	std::vector<FileInfoList> lists;
	takeSnapshots( watch, lists );
	rootWatch->SharedActionBatch->addSourceTree( directory, std::move( lists ) );
}

void WatcherKqueue::takeSnapshots( Watcher* watch, std::vector<FileInfoList>& lists ) {
	if ( WatcherKqueue* kqueueWatch = dynamic_cast<WatcherKqueue*>( watch ) ) {
		lists.push_back( std::move( kqueueWatch->mDirSnap.Files ) );
		for ( const auto& child : kqueueWatch->mWatches )
			takeSnapshots( child.second, lists );
	} else if ( WatcherGeneric* genericWatch = dynamic_cast<WatcherGeneric*>( watch ) ) {
		genericWatch->takeSnapshots( lists );
	}
}

bool WatcherKqueue::collectTree( const std::string& directory,
								 std::vector<const FileInfo*>* entries ) {
	std::string path( directory );
	FileSystem::dirAddSlashAtEnd( path );
	if ( path != Directory ) {
		// The watch below this one whose directory contains it.
		for ( const auto& child : mWatches )
			if ( 0 == path.compare( 0, child.second->Directory.size(), child.second->Directory ) )
				return child.second->collectTree( path, entries );
		return false;
	}
	if ( NULL != entries ) {
		for ( const FileInfo& entry : mDirSnap.Files )
			entries->push_back( &entry );
		for ( const auto& child : mWatches )
			child.second->collectTree( child.second->Directory, entries );
	}
	return true;
}

bool WatcherKqueue::snapshotHasAny( const std::set<FileIdentity>& identities ) const {
	if ( mDirSnap.hasAny( identities ) )
		return true;
	for ( const auto& child : mWatches ) {
		if ( child.second->snapshotHasAny( identities ) )
			return true;
	}
	return false;
}

void WatcherKqueue::reportDirectories() {
	for ( const auto& entry : mWatches ) {
		WatcherKqueue* child = dynamic_cast<WatcherKqueue*>( entry.second );
		if ( child ) {
			handleFolderAction( entry.second->Directory, Actions::Add, "",
								child->mDirSnap.DirectoryInfo );
			child->reportDirectories();
		} else {
			handleFolderAction( entry.second->Directory, Actions::Add, "",
								FileInfo( entry.second->Directory ) );
		}
	}
}

WatcherKqueue* WatcherKqueue::root() {
	WatcherKqueue* watch = this;
	while ( watch->mParent )
		watch = watch->mParent;
	return watch;
}

void WatcherKqueue::rescanTree() {
	rescan();
	for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
		WatcherKqueue* watch = dynamic_cast<WatcherKqueue*>( it->second );
		if ( NULL != watch )
			watch->rescanTree();
		else
			it->second->watch();
	}
}

Watcher* WatcherKqueue::findWatcher( std::string path ) {
	// Watch directories end with a slash; snapshot entries, like deleted directories, do not.
	FileSystem::dirAddSlashAtEnd( path );
	WatchMap::iterator it = mWatches.begin();

	for ( ; it != mWatches.end(); it++ ) {
		if ( it->second->Directory == path ) {
			return it->second;
		}
	}

	return NULL;
}

void WatcherKqueue::moveDirectory( std::string oldPath, std::string newPath,
								   const FileInfo& fileInfo, bool emitEvents ) {
	// Update the directory path if it's a watcher
	std::string opath2( oldPath );
	FileSystem::dirAddSlashAtEnd( opath2 );

	Watcher* watch = findWatcher( opath2 );

	if ( NULL != watch ) {
		std::string npath( newPath );
		FileSystem::dirAddSlashAtEnd( npath );

		// Subdirectories are watched by kqueue watches, or by generic watches when file
		// descriptors ran out, which update their own tree.
		std::vector<std::pair<WatcherKqueue*, std::string>> stack;
		auto moveWatch = [&stack]( Watcher* moved, const std::string& path ) {
			if ( WatcherKqueue* kqueueWatch = dynamic_cast<WatcherKqueue*>( moved ) )
				stack.push_back( std::make_pair( kqueueWatch, path ) );
			else if ( WatcherGeneric* genericWatch = dynamic_cast<WatcherGeneric*>( moved ) )
				genericWatch->moveDirectory( path );
		};
		moveWatch( watch, npath );

		while ( !stack.empty() ) {
			WatcherKqueue* wkq = stack.back().first;
			std::string curNPath = stack.back().second;
			stack.pop_back();

			wkq->Directory = curNPath;
			wkq->mDirSnap.setDirectoryInfo( curNPath );
			for ( size_t i = 1; i <= wkq->mChangeListCount; i++ ) {
				if ( NULL != wkq->mChangeList[i].udata ) {
					FileInfo* fi = reinterpret_cast<FileInfo*>( wkq->mChangeList[i].udata );
					std::string filename = FileSystem::fileNameFromPath( fi->Filepath );
					fi->Filepath = curNPath + filename;
				}
			}
			qsort( &wkq->mChangeList[1], wkq->mChangeListCount, sizeof( KEvent ), comparator );

			for ( auto& childPair : wkq->mWatches ) {
				std::string childFolderName =
					FileSystem::fileNameFromPath( childPair.second->Directory );
				std::string childNewPath = curNPath + childFolderName;
				FileSystem::dirAddSlashAtEnd( childNewPath );
				moveWatch( childPair.second, childNewPath );
			}
		}
	}

	if ( emitEvents ) {
		handleFolderAction( newPath, efsw::Actions::Moved, oldPath, fileInfo );
	}
}

WatchID WatcherKqueue::addWatch( const std::string& directory, FileWatchListener* watcher,
								 bool recursive, WatcherKqueue* parent ) {
	std::string dir( directory );

	FileSystem::dirAddSlashAtEnd( dir );

	// This should never happen here
	if ( !FileSystem::isDirectory( dir ) ) {
		return Errors::Log::createLastError( Errors::FileNotFound, dir );
	} else if ( pathInWatches( dir ) || pathInParent( dir ) ) {
		return Errors::Log::createLastError( Errors::FileRepeated, directory );
	} else if ( NULL != parent && FileSystem::isRemoteFS( dir ) ) {
		return Errors::Log::createLastError( Errors::FileRemote, dir );
	}

	std::string curPath;
	std::string link( FileSystem::getLinkRealPath( dir, curPath ) );

	if ( "" != link ) {
		/// Avoid adding symlinks directories if it's now enabled
		if ( NULL != parent && !mWatcher->mFileWatcher->followSymlinks() ) {
			return Errors::Log::createLastError( Errors::FileOutOfScope, dir );
		}

		if ( pathInWatches( link ) || pathInParent( link ) ) {
			return Errors::Log::createLastError( Errors::FileRepeated, link );
		} else if ( !mWatcher->linkAllowed( curPath, link ) ) {
			return Errors::Log::createLastError( Errors::FileOutOfScope, link );
		} else {
			dir = link;
		}
	}

	if ( mWatcher->availablesFD() ) {
		WatcherKqueue* watch = new WatcherKqueue( ++mLastWatchID, dir, watcher, recursive, mWatcher,
												  parent, mReportCrossDirectoryMoves );

		mWatches.insert( std::make_pair( mLastWatchID, watch ) );

		watch->addAll();

		// if failed to open the directory... erase the watcher
		if ( !watch->initOK() ) {
			int le = watch->lastErrno();

			mWatches.erase( watch->ID );

			efSAFE_DELETE( watch );

			mLastWatchID--;

			// Probably the folder has too many files, create a generic watcher
			if ( EACCES != le ) {
				addGenericWatch( dir, watcher, recursive );
			} else {
				return Errors::Log::createLastError( Errors::Unspecified, link );
			}
		}
	} else {
		efDEBUG( "Using a generic watcher, file descriptor limit reached: %ld\n",
				 mWatcher->mFileDescriptorCount.load() );

		addGenericWatch( dir, watcher, recursive );
	}

	return mLastWatchID;
}

void WatcherKqueue::addGenericWatch( const std::string& dir, FileWatchListener* watcher,
									 bool recursive ) {
	WatcherGeneric* watch = new WatcherGeneric( ++mLastWatchID, dir, watcher, mWatcher, recursive,
												mReportCrossDirectoryMoves );
	// It reports like the rest of its tree: into the root's shared batch, as part of the root's
	// registration, so that its moves to and from other registrations are paired.
	WatcherKqueue* rootWatch = root();
	watch->SharedActionBatch = rootWatch->SharedActionBatch;
	watch->SharedActionOwner = rootWatch;
	mWatches.insert( std::make_pair( mLastWatchID, watch ) );
}

bool WatcherKqueue::initOK() {
	return mInitOK;
}

void WatcherKqueue::removeWatch( WatchID watchid ) {
	WatchMap::iterator iter = mWatches.find( watchid );

	if ( iter == mWatches.end() )
		return;

	Watcher* watch = iter->second;

	mWatches.erase( iter );

	efSAFE_DELETE( watch );
}

bool WatcherKqueue::pathInWatches( const std::string& path ) {
	return NULL != findWatcher( path );
}

bool WatcherKqueue::pathInParent( const std::string& path ) {
	WatcherKqueue* pNext = mParent;

	while ( NULL != pNext ) {
		if ( pNext->pathInWatches( path ) ) {
			return true;
		}

		pNext = pNext->mParent;
	}

	if ( mWatcher->pathInWatches( path ) ) {
		return true;
	}

	if ( path == Directory ) {
		return true;
	}

	return false;
}

int WatcherKqueue::lastErrno() {
	return mErrno;
}

} // namespace efsw

#endif
