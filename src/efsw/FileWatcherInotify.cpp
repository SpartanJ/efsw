#include <algorithm>
#include <efsw/FileWatcherInotify.hpp>
#include <utility>

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include <efsw/Debug.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/Lock.hpp>
#include <efsw/String.hpp>
#include <efsw/System.hpp>

#define BUFF_SIZE ( ( sizeof( struct inotify_event ) + FILENAME_MAX ) * 1024 )

namespace efsw {

FileWatcherInotify::FileWatcherInotify( FileWatcher* parent ) :
	FileWatcherImpl( parent ), mFD( -1 ), mThread( NULL ), mIsTakingAction( false ) {
	mFD = inotify_init();

	if ( mFD < 0 ) {
		efDEBUG( "Error: %s\n", strerror( errno ) );
	} else {
		mInitOK = true;
	}
}

FileWatcherInotify::~FileWatcherInotify() {
	mInitOK = false;
	// There is deadlock when release FileWatcherInotify instance since its handAction
	// function is still running and hangs in requiring lock without init lock captured.
	while ( mIsTakingAction ) {
		// It'd use condition-wait instead of sleep. Actually efsw has no such
		// implementation so we just skip and sleep while for that to avoid deadlock.
		usleep( 1000 );
	};
	Lock initLock( mInitLock );

	efSAFE_DELETE( mThread );

	Lock l( mWatchesLock );
	Lock l2( mRealWatchesLock );

	WatchMap::iterator iter = mWatches.begin();
	WatchMap::iterator end = mWatches.end();

	for ( ; iter != end; ++iter ) {
		efSAFE_DELETE( iter->second );
	}

	mWatches.clear();

	for ( auto w : mDeletedWatches ) {
		efSAFE_DELETE( w );
	}
	mDeletedWatches.clear();

	if ( mFD != -1 ) {
		close( mFD );
		mFD = -1;
	}
}

WatchID FileWatcherInotify::addWatch( const std::string& directory, FileWatchListener* watcher,
									  bool recursive, const std::vector<WatcherOption>& options ) {
	if ( !mInitOK )
		return Errors::Log::createLastError( Errors::Unspecified, directory );
	Lock initLock( mInitLock );

	bool syntheticEvents = getOptionValue( options, Options::LinuxProduceSyntheticEvents, 0 ) != 0;
	bool reportCrossDirectoryMoves =
		getOptionValue( options, Options::ReportCrossDirectoryMoves, 0 ) != 0;
	return addWatch( directory, watcher, recursive, syntheticEvents, reportCrossDirectoryMoves,
					 NULL );
}

WatchID FileWatcherInotify::addWatch( const std::string& directory, FileWatchListener* watcher,
									  bool recursive, bool syntheticEvents,
									  bool reportCrossDirectoryMoves, WatcherInotify* parent,
									  bool fromInternalEvent ) {
	std::string dir( directory );

	FileSystem::dirAddSlashAtEnd( dir );

	FileInfo fi( dir );

	if ( !fi.isDirectory() ) {
		return Errors::Log::createLastError( Errors::FileNotFound, dir );
	} else if ( !fi.isReadable() ) {
		return Errors::Log::createLastError( Errors::FileNotReadable, dir );
	} else if ( NULL != parent && pathInWatches( dir ) ) {
		/// Internal recursive watches keep the exact-duplicate check: a newly
		/// traversed subdirectory must not register over an existing explicit
		/// watch root.
		return Errors::Log::createLastError( Errors::FileRepeated, directory );
	} else if ( NULL != parent && FileSystem::isRemoteFS( dir ) ) {
		return Errors::Log::createLastError( Errors::FileRemote, dir );
	}

	/// Check if the directory is a symbolic link
	std::string curPath;
	std::string link( FileSystem::getLinkRealPath( dir, curPath ) );

	if ( "" != link ) {
		/// Avoid adding symlinks directories if it's now enabled
		if ( NULL != parent && !mFileWatcher->followSymlinks() ) {
			return Errors::Log::createLastError( Errors::FileOutOfScope, dir );
		}

		/// If it's a symlink check if the realpath exists as a watcher, or
		/// if the path is outside the current dir.
		/// For explicit user watches the conflict check runs on the resolved
		/// link target, so that the comparison sees the same normalized path
		/// that will be registered with inotify.
		if ( !linkAllowed( curPath, link ) ) {
			return Errors::Log::createLastError( Errors::FileOutOfScope, dir );
		} else if ( NULL != parent && pathInWatches( link ) ) {
			return Errors::Log::createLastError( Errors::FileRepeated, directory );
		} else {
			dir = link;
		}
	}

	if ( NULL == parent ) {
		const Errors::Error conflict = getWatchConflict( dir, recursive );
		if ( Errors::NoError != conflict ) {
			/// Explicit user watches must not overlap an existing registration in a
			/// way that would require both to own the same native inotify directory
			/// watch. Compare only after resolving a symlink so the check uses the
			/// effective directory that will be registered with inotify.
			return Errors::Log::createLastError( conflict, directory );
		}
	}

	int wd = inotify_add_watch( mFD, dir.c_str(),
								IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_MOVED_FROM |
									IN_DELETE | IN_MODIFY );

	if ( wd < 0 ) {
		if ( errno == ENOENT ) {
			return Errors::Log::createLastError( Errors::FileNotFound, dir );
		} else {
			return Errors::Log::createLastError( Errors::Unspecified,
												 std::string( strerror( errno ) ) );
		}
	}

	// Explicit overlapping user watches are rejected before calling
	// inotify_add_watch(). Therefore an existing wd here must not represent
	// a second explicit registration of the same physical directory.
	//
	// This path is retained for inotify identity reuse caused by filesystem
	// topology changes (for example, an already watched directory being moved).
	{
		Lock lock( mWatchesLock );
		auto watchIdExists = mWatches.find( wd );
		if ( watchIdExists != mWatches.end() )
			removeWatchLocked( wd, true );
	}

	efDEBUG( "Added watch %s with id: %d\n", dir.c_str(), wd );

	WatcherInotify* pWatch = new WatcherInotify();
	pWatch->Listener = watcher;
	pWatch->ID = parent ? parent->ID : wd;
	pWatch->InotifyID = wd;
	pWatch->Directory = dir;
	pWatch->Recursive = recursive;
	pWatch->Parent = parent;
	pWatch->syntheticEvents = syntheticEvents;
	pWatch->reportCrossDirectoryMoves = reportCrossDirectoryMoves;

	{
		Lock lock( mWatchesLock );
		mWatches[wd] = pWatch;
		mWatchesRef[pWatch->Directory] = wd;
	}

	if ( NULL == pWatch->Parent ) {
		Lock l( mRealWatchesLock );
		mRealWatches[pWatch->InotifyID] = pWatch;
	}

	if ( pWatch->Recursive ) {
		FileInfoList files = FileSystem::filesInfoFromPath( pWatch->Directory );

		if ( fromInternalEvent && parent != NULL && syntheticEvents ) {
			for ( const auto& file : files ) {
				if ( file.isRegularFile() || file.isDirectory() || file.isLink() ) {
					pWatch->Listener->handleFileAction(
						pWatch->ID, pWatch->Directory,
						FileSystem::fileNameFromPath( file.Filepath ), Actions::Add );
				}
			}
		}

		for ( const auto& cfi : files ) {
			if ( !mInitOK )
				break;

			if ( cfi.isDirectory() && cfi.isReadable() ) {
				addWatch( cfi.Filepath, watcher, recursive, syntheticEvents,
						  reportCrossDirectoryMoves, pWatch, fromInternalEvent );
			}
		}
	}

	return wd;
}

void FileWatcherInotify::removeWatchLocked( WatchID watchid, bool skipInotifyRemove ) {
	WatchMap::iterator iter = mWatches.find( watchid );
	if ( iter == mWatches.end() )
		return;

	WatcherInotify* watch = iter->second;

	if ( watch->Recursive && NULL == watch->Parent ) {
		WatchMap::iterator it = mWatches.begin();
		std::vector<WatchID> eraseWatches;

		for ( ; it != mWatches.end(); ++it )
			if ( it->second != watch && it->second->inParentTree( watch ) )
				eraseWatches.push_back( it->second->InotifyID );

		for ( std::vector<WatchID>::iterator eit = eraseWatches.begin(); eit != eraseWatches.end();
			  ++eit ) {
			removeWatch( *eit );
		}
	}

	mWatchesRef.erase( watch->Directory );
	mWatches.erase( iter );

	if ( NULL == watch->Parent ) {
		WatchMap::iterator eraseit = mRealWatches.find( watch->InotifyID );

		if ( eraseit != mRealWatches.end() ) {
			mRealWatches.erase( eraseit );
		}
	}

	if ( !skipInotifyRemove ) {
		int err = inotify_rm_watch( mFD, watchid );

		if ( err < 0 ) {
			efDEBUG( "Error removing watch %d: %s\n", watchid, strerror( errno ) );
		} else {
			efDEBUG( "Removed watch %s with id: %d\n", watch->Directory.c_str(), watchid );
		}
	}

	mDeletedWatches.push_back( watch );
}

void FileWatcherInotify::removeWatch( const std::string& directory ) {
	if ( !mInitOK )
		return;
	Lock initLock( mInitLock );
	Lock lock( mWatchesLock );
	Lock l( mRealWatchesLock );

	std::string dir( directory );
	FileSystem::dirAddSlashAtEnd( dir );

	std::unordered_map<std::string, WatchID>::iterator ref = mWatchesRef.find( dir );
	if ( ref == mWatchesRef.end() )
		return;

	removeWatchLocked( ref->second );
}

void FileWatcherInotify::removeWatch( WatchID watchid ) {
	if ( !mInitOK )
		return;
	Lock initLock( mInitLock );
	Lock lock( mWatchesLock );
	removeWatchLocked( watchid );
}

void FileWatcherInotify::watch() {
	if ( NULL == mThread ) {
		mThread = new Thread( [this] { run(); } );
		mThread->launch();
	}
}

Watcher* FileWatcherInotify::watcherContainsDirectory( std::string dir ) {
	FileSystem::dirRemoveSlashAtEnd( dir );
	std::string watcherPath = FileSystem::pathRemoveFileName( dir );
	FileSystem::dirAddSlashAtEnd( watcherPath );
	Lock lock( mWatchesLock );

	for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
		Watcher* watcher = it->second;
		if ( watcher->Directory == watcherPath )
			return watcher;
	}

	return NULL;
}

void FileWatcherInotify::run() {
	// inotify reports a rename as two events connected by a cookie. The pair is
	// normally consecutive, but Linux does not guarantee that: unrelated events
	// and other move pairs may occur between IN_MOVED_FROM and IN_MOVED_TO.
	//
	// Parse a complete read batch before delivering callbacks so cookie pairs can
	// be identified without changing the order of unrelated events. We only
	// coalesce pairs fully contained in this batch. Retaining a source across
	// reads would require retaining every newer event too; otherwise a delayed
	// Delete or Moved could be delivered after an event that invalidated it.
	struct BatchEvent {
		int wd;
		uint32_t mask;
		uint32_t cookie;
		std::string filename;
	};

	struct MovePair {
		MovePair() : destinationWd( -1 ) {}

		int destinationWd;
	};

	struct PendingMove {
		WatcherInotify* watcher;
		std::string filename;
	};
	// Keep the bucket array between read batches. clear() destroys the nodes but
	// does not normally release the buckets, avoiding a complete map allocation
	// on every batch while retaining average constant-time cookie lookup.
	std::unordered_map<uint32_t, PendingMove> pendingMoves;
	pendingMoves.reserve( 16 );
	std::vector<BatchEvent> events;
	events.reserve( 64 );
	std::unordered_map<uint32_t, MovePair> movePairs;
	movePairs.reserve( 16 );

	char* buff = new char[BUFF_SIZE];
	memset( buff, 0, BUFF_SIZE );

	// Report a source whose destination cannot be represented as one Moved event.
	// Directories require extra cleanup because every recursively watched child
	// has its own native inotify watch.
	auto emitMovedOutside = [this]( WatcherInotify* watch, const std::string& oldFileName ) {
		watch->OldFileName.clear();

		std::vector<Watcher*> eraseWatches;
		{
			Lock lock( mWatchesLock );
			for ( const auto& entry : mWatches ) {
				Watcher* oldWatch = entry.second;
				if ( oldWatch != watch &&
					 -1 != String::strStartsWith( watch->Directory + oldFileName + "/",
												  oldWatch->Directory ) )
					eraseWatches.push_back( oldWatch );
			}
		}

		std::stable_sort( eraseWatches.begin(), eraseWatches.end(),
						  []( const Watcher* left, const Watcher* right ) {
							  return left->Directory < right->Directory;
						  } );

		if ( eraseWatches.empty() ) {
			handleAction( watch, oldFileName, IN_DELETE );
		} else {
			for ( auto it = eraseWatches.rbegin(); it != eraseWatches.rend(); ++it ) {
				Watcher* rmWatch = *it;
				if ( Watcher* containingWatch = watcherContainsDirectory( rmWatch->Directory ) ) {
					handleAction( containingWatch,
								  FileSystem::fileNameFromPath( rmWatch->Directory ), IN_DELETE );
				}
			}
		}
	};

	do {
		fd_set rfds;
		FD_ZERO( &rfds );
		FD_SET( mFD, &rfds );
		timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 100000;

		if ( select( FD_SETSIZE, &rfds, NULL, NULL, &timeout ) > 0 ) {
			ssize_t len;

			len = read( mFD, buff, BUFF_SIZE );

			if ( len != -1 ) {
				events.clear();
				movePairs.clear();
				ssize_t i = 0;

				while ( i < len ) {
					struct inotify_event* pevent = (struct inotify_event*)&buff[i];
					events.push_back( BatchEvent{ pevent->wd, pevent->mask, pevent->cookie,
												  std::string( (char*)pevent->name ) } );

					if ( pevent->mask & IN_MOVED_TO )
						movePairs[pevent->cookie].destinationWd = pevent->wd;

					i += sizeof( struct inotify_event ) + pevent->len;
				}

				// Replay in kernel queue order. A source is deferred only when the
				// complete pair will be represented by one Moved callback. Otherwise
				// Delete is emitted at FROM and Add at TO, preserving their positions.
				pendingMoves.clear();

				for ( const auto& event : events ) {
					WatcherInotify* curWatcher = NULL;
					{
						Lock lock( mWatchesLock );
						auto watcher = mWatches.find( event.wd );
						if ( watcher != mWatches.end() )
							curWatcher = watcher->second;
					}

					if ( event.mask & IN_MOVED_FROM ) {
						if ( !curWatcher )
							continue;

						WatcherInotify* destination = NULL;
						auto pair = movePairs.find( event.cookie );
						if ( pair != movePairs.end() && pair->second.destinationWd != -1 ) {
							Lock lock( mWatchesLock );
							auto watcher = mWatches.find( pair->second.destinationWd );
							if ( watcher != mWatches.end() )
								destination = watcher->second;
						}

						const bool coalesce =
							destination && ( destination == curWatcher ||
											 ( destination->reportCrossDirectoryMoves &&
											   destination->ID == curWatcher->ID ) );
						if ( coalesce ) {
							pendingMoves.erase( event.cookie );
							pendingMoves.emplace( event.cookie,
												  PendingMove{ curWatcher, event.filename } );
						} else {
							emitMovedOutside( curWatcher, event.filename );
						}
					} else if ( event.mask & IN_MOVED_TO ) {
						auto pending = pendingMoves.find( event.cookie );
						if ( pending == pendingMoves.end() ) {
							if ( curWatcher )
								handleAction( curWatcher, event.filename, event.mask );
							continue;
						}

						WatcherInotify* source = pending->second.watcher;
						std::string sourceFilename = std::move( pending->second.filename );
						pendingMoves.erase( pending );

						if ( !curWatcher ) {
							emitMovedOutside( source, sourceFilename );
						} else if ( source == curWatcher ) {
							curWatcher->OldFileName = sourceFilename;
							handleAction( curWatcher, event.filename, event.mask );
						} else {
							emitCrossDirectoryMove( source, sourceFilename, curWatcher,
													event.filename );
						}
					} else if ( curWatcher ) {
						handleAction( curWatcher, event.filename, event.mask );
					}
				}
			}
		}

		{
			Lock lock( mWatchesLock );
			if ( !mDeletedWatches.empty() ) {
				for ( auto w : mDeletedWatches ) {
					efSAFE_DELETE( w );
				}
				mDeletedWatches.clear();
			}
		}
	} while ( mInitOK );

	delete[] buff;
}

void FileWatcherInotify::checkForNewWatcher( Watcher* watch, std::string fpath ) {
	FileSystem::dirAddSlashAtEnd( fpath );

	/// If the watcher is recursive, checks if the new file is a folder, and creates a watcher
	if ( watch->Recursive && FileSystem::isDirectory( fpath ) ) {
		bool found = false;

		{
			Lock lock( mWatchesLock );

			/// First check if exists
			for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
				if ( it->second->Directory == fpath ) {
					found = true;
					break;
				}
			}
		}

		if ( !found ) {
			WatcherInotify* iWatch = static_cast<WatcherInotify*>( watch );
			addWatch( fpath, watch->Listener, watch->Recursive, iWatch->syntheticEvents,
					  iWatch->reportCrossDirectoryMoves, static_cast<WatcherInotify*>( watch ),
					  true );
		}
	}
}

void FileWatcherInotify::emitCrossDirectoryMove( Watcher* src, const std::string& srcFile,
												 Watcher* dst, const std::string& dstFile ) {
	if ( !src || !dst ) {
		return;
	}

	std::string oldDstFilename = dst->OldFileName;
	dst->OldFileName = src->Directory + srcFile;
	handleAction( dst, dstFile, IN_MOVED_TO );
	dst->OldFileName = std::move( oldDstFilename );
}

void FileWatcherInotify::handleAction( Watcher* watch, const std::string& filename,
									   unsigned long action, const std::string& ) {
	if ( !watch || !watch->Listener || !mInitOK ) {
		return;
	}
	mIsTakingAction = true;
	Lock initLock( mInitLock );

	std::string fpath( watch->Directory + filename );

	if ( IN_Q_OVERFLOW & action ) {
		watch->Listener->handleMissedFileActions( watch->ID, watch->Directory );
	} else if ( ( IN_CLOSE_WRITE & action ) || ( IN_MODIFY & action ) ) {
		watch->Listener->handleFileAction( watch->ID, watch->Directory, filename,
										   Actions::Modified );
	} else if ( IN_MOVED_TO & action ) {
		/// If OldFileName doesn't exist means that the file has been moved from other folder, so we
		/// just send the Add event
		if ( watch->OldFileName.empty() ) {
			// Install recursive directory coverage before notifying the listener. A
			// listener can react to Add immediately and create files in the new
			// directory, so publishing the event first leaves a window where those
			// files are missed by inotify.
			checkForNewWatcher( watch, fpath );

			watch->Listener->handleFileAction( watch->ID, watch->Directory, filename,
											   Actions::Add );

			watch->Listener->handleFileAction( watch->ID, watch->Directory, filename,
											   Actions::Modified );
		} else {
			watch->Listener->handleFileAction( watch->ID, watch->Directory, filename,
											   Actions::Moved, watch->OldFileName );
		}

		if ( watch->Recursive && FileSystem::isDirectory( fpath ) && !watch->OldFileName.empty() ) {
			/// Update the new directory path
			std::string opath( watch->OldFileName );
			if ( opath.empty() || opath[0] != FileSystem::getOSSlash() )
				opath = watch->Directory + opath;
			FileSystem::dirAddSlashAtEnd( opath );
			FileSystem::dirAddSlashAtEnd( fpath );

			Lock lock( mWatchesLock );

			for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
				if ( it->second->Directory == opath ) {
					it->second->Directory = fpath;
					it->second->DirInfo = FileInfo( fpath );
				} else if ( -1 != String::strStartsWith( opath, it->second->Directory ) ) {
					it->second->Directory = fpath + it->second->Directory.substr( opath.size() );
					it->second->DirInfo.Filepath = it->second->Directory;
				}
			}
		}

		watch->OldFileName = "";
	} else if ( IN_CREATE & action ) {
		checkForNewWatcher( watch, fpath );

		watch->Listener->handleFileAction( watch->ID, watch->Directory, filename, Actions::Add );
	} else if ( IN_MOVED_FROM & action ) {
		watch->OldFileName = filename;
	} else if ( IN_DELETE & action ) {
		watch->Listener->handleFileAction( watch->ID, watch->Directory, filename, Actions::Delete );

		FileSystem::dirAddSlashAtEnd( fpath );

		/// If the file erased is a directory and recursive is enabled, removes the directory erased
		if ( watch->Recursive ) {
			Lock l( mWatchesLock );

			for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
				if ( it->second->Directory == fpath ) {
					removeWatchLocked( it->second->InotifyID );
					break;
				}
			}
		}
	}
	mIsTakingAction = false;
}

std::vector<std::string> FileWatcherInotify::directories() {
	std::vector<std::string> dirs;

	Lock l( mRealWatchesLock );

	dirs.reserve( mRealWatches.size() );

	WatchMap::iterator it = mRealWatches.begin();

	for ( ; it != mRealWatches.end(); ++it )
		dirs.push_back( it->second->Directory );

	return dirs;
}

bool FileWatcherInotify::isDirectoryInside( const std::string& child, const std::string& parent ) {
	if ( parent.empty() )
		return false;

	if ( child == parent )
		return false;

	if ( child.size() <= parent.size() )
		return false;

	if ( child.compare( 0, parent.size(), parent ) != 0 )
		return false;

	return FileSystem::slashAtEnd( parent ) || child[parent.size()] == FileSystem::getOSSlash();
}

std::string FileWatcherInotify::directoryComparisonPath( const std::string& directory ) {
	std::string comparisonPath( directory );

	// inotify identifies the filesystem object, not the spelling supplied by
	// the caller. Resolve '.', '..', relative paths, and symlink aliases before
	// deciding whether two explicit registrations overlap.
	FileInfo fileInfo( comparisonPath );
	if ( fileInfo.isDirectory() ) {
		std::string realPath( FileSystem::getRealPath( comparisonPath ) );
		if ( !realPath.empty() )
			comparisonPath = std::move( realPath );
	}

	FileSystem::dirAddSlashAtEnd( comparisonPath );
	return comparisonPath;
}

Errors::Error FileWatcherInotify::getWatchConflict( const std::string& directory, bool recursive ) {
	Lock l( mRealWatchesLock );
	const std::string comparisonDirectory( directoryComparisonPath( directory ) );

	for ( const auto& entry : mRealWatches ) {
		const WatcherInotify* watch = entry.second;

		if ( NULL == watch )
			continue;

		const std::string existingDirectory( directoryComparisonPath( watch->Directory ) );

		if ( existingDirectory == comparisonDirectory )
			return Errors::FileRepeated;

		if ( watch->Recursive && isDirectoryInside( comparisonDirectory, existingDirectory ) )
			return Errors::FileOverlapping;

		if ( recursive && isDirectoryInside( existingDirectory, comparisonDirectory ) )
			return Errors::FileOverlapping;
	}

	return Errors::NoError;
}

bool FileWatcherInotify::pathInWatches( const std::string& path ) {
	Lock l( mRealWatchesLock );

	/// pathInWatches() checks exact explicit user watches only.
	/// Overlap with recursive explicit roots is validated separately by
	/// getWatchConflict() before a new root is registered.
	WatchMap::iterator it = mRealWatches.begin();

	for ( ; it != mRealWatches.end(); ++it )
		if ( it->second->Directory == path )
			return true;

	return false;
}

} // namespace efsw

#endif
