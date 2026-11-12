#include <algorithm>
#include <efsw/FileWatcherInotify.hpp>
#include <utility>

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
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
	FileWatcherImpl( parent ), mFD( -1 ), mThread( NULL ) {
	mFD = inotify_init();

	if ( mFD < 0 ) {
		efDEBUG( "Error: %s\n", strerror( errno ) );
	} else {
		mInitOK = true;
	}
}

FileWatcherInotify::~FileWatcherInotify() {
	mInitOK = false;
	// Do not hold mInitLock while joining: an in-flight action may still need it.
	efSAFE_DELETE( mThread );

	Lock l( mWatchesLock );
	Lock l2( mRealWatchesLock );

	for ( auto& native : mWatches ) {
		for ( WatcherInotify* watch : native.second )
			delete watch;
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

	// inotify returns the existing descriptor when the inode is already watched. Explicit
	// overlapping registrations are rejected above, so this only happens after topology
	// changes, for example when a watched directory was moved into a recursive registration.
	{
		Lock lock( mWatchesLock );
		std::vector<WatcherInotify*> owners;
		ownersLocked( wd, owners );
		for ( WatcherInotify* owner : owners ) {
			// The same directory reached through another path is still one registration.
			if ( NULL == parent && NULL == owner->Parent )
				return Errors::Log::createLastError( Errors::FileRepeated, directory );
			if ( NULL != parent && owner->ID == parent->ID ) {
				// This registration already watches the directory under a stale path.
				if ( NULL == owner->Parent )
					return Errors::Log::createLastError( Errors::FileRepeated, directory );
				removeWatchLocked( owner, true );
			}
		}
		// Owners from other registrations keep the descriptor; this watch joins them.
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
		registerWatchLocked( pWatch );
	}

	if ( NULL == pWatch->Parent ) {
		Lock l( mRealWatchesLock );
		mRealWatches[pWatch->ID] = pWatch;
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

void FileWatcherInotify::registerWatchLocked( WatcherInotify* watch ) {
	mWatches[watch->InotifyID].push_back( watch );
	mWatchesRef.emplace( watch->Directory, watch );
}

void FileWatcherInotify::unindexWatchLocked( WatcherInotify* watch ) {
	auto range = mWatchesRef.equal_range( watch->Directory );
	for ( auto it = range.first; it != range.second; ++it ) {
		if ( it->second == watch ) {
			mWatchesRef.erase( it );
			return;
		}
	}
}

void FileWatcherInotify::ownersLocked( int wd, std::vector<WatcherInotify*>& owners ) const {
	owners.clear();
	auto native = mWatches.find( wd );
	if ( native != mWatches.end() )
		owners.insert( owners.end(), native->second.begin(), native->second.end() );
}

WatcherInotify* FileWatcherInotify::findWatchLocked( const std::string& directory,
													 WatchID registration ) {
	auto range = mWatchesRef.equal_range( directory );
	for ( auto it = range.first; it != range.second; ++it ) {
		if ( it->second->ID == registration )
			return it->second;
	}
	return NULL;
}

void FileWatcherInotify::removeWatchLocked( WatcherInotify* watch, bool skipInotifyRemove ) {
	if ( watch->Removed )
		return;

	// Every watch below this one belongs to the same registration and must go with it;
	// otherwise their Parent chain would point to a deleted watch. They are all found in one
	// pass: searching again for the descendants of each removed watch would be quadratic.
	std::vector<WatcherInotify*> children;
	for ( const auto& native : mWatches ) {
		for ( WatcherInotify* child : native.second ) {
			if ( child != watch && !child->Removed && child->inParentTree( watch ) )
				children.push_back( child );
		}
	}

	unregisterWatchLocked( watch, skipInotifyRemove );
	for ( WatcherInotify* child : children )
		unregisterWatchLocked( child, false );
}

void FileWatcherInotify::unregisterWatchLocked( WatcherInotify* watch, bool skipInotifyRemove ) {
	watch->Removed = true;
	unindexWatchLocked( watch );

	auto native = mWatches.find( watch->InotifyID );
	if ( native != mWatches.end() ) {
		std::vector<WatcherInotify*>& owners = native->second;
		owners.erase( std::remove( owners.begin(), owners.end(), watch ), owners.end() );
		if ( owners.empty() ) {
			mWatches.erase( native );
			if ( !skipInotifyRemove ) {
				if ( inotify_rm_watch( mFD, watch->InotifyID ) < 0 ) {
					efDEBUG( "Error removing watch %d: %s\n", watch->InotifyID, strerror( errno ) );
				} else {
					efDEBUG( "Removed watch %s with id: %d\n", watch->Directory.c_str(),
							 watch->InotifyID );
				}
			}
		}
	}

	if ( NULL == watch->Parent ) {
		Lock l( mRealWatchesLock );
		auto real = mRealWatches.find( watch->ID );
		if ( real != mRealWatches.end() && real->second == watch )
			mRealWatches.erase( real );
	}

	mDeletedWatches.push_back( watch );
}

void FileWatcherInotify::removeWatch( const std::string& directory ) {
	if ( !mInitOK )
		return;
	Lock initLock( mInitLock );
	Lock lock( mWatchesLock );

	std::string dir( directory );
	FileSystem::dirAddSlashAtEnd( dir );

	// Prefer the explicit registration when other registrations share the directory.
	WatcherInotify* watch = NULL;
	{
		Lock l( mRealWatchesLock );
		for ( const auto& real : mRealWatches ) {
			if ( real.second->Directory == dir ) {
				watch = real.second;
				break;
			}
		}
	}
	if ( NULL == watch ) {
		auto ref = mWatchesRef.find( dir );
		if ( ref == mWatchesRef.end() )
			return;
		watch = ref->second;
	}

	removeWatchLocked( watch );
}

void FileWatcherInotify::removeWatch( WatchID watchid ) {
	if ( !mInitOK )
		return;
	Lock initLock( mInitLock );
	Lock lock( mWatchesLock );
	WatcherInotify* watch = NULL;
	{
		Lock l( mRealWatchesLock );
		auto real = mRealWatches.find( watchid );
		if ( real == mRealWatches.end() )
			return;
		watch = real->second;
	}
	removeWatchLocked( watch );
}

void FileWatcherInotify::watch() {
	if ( NULL == mThread ) {
		mThread = new Thread( [this] { run(); } );
		mThread->launch();
	}
}

Watcher* FileWatcherInotify::watcherContainsDirectory( std::string dir, WatchID registration ) {
	FileSystem::dirRemoveSlashAtEnd( dir );
	std::string watcherPath = FileSystem::pathRemoveFileName( dir );
	FileSystem::dirAddSlashAtEnd( watcherPath );
	Lock lock( mWatchesLock );
	return findWatchLocked( watcherPath, registration );
}

void FileWatcherInotify::renameWatchedDirectoriesLocked( const std::string& oldPath,
														 const std::string& newPath ) {
	for ( auto& native : mWatches ) {
		for ( WatcherInotify* moved : native.second ) {
			if ( -1 == String::strStartsWith( oldPath, moved->Directory ) )
				continue;
			unindexWatchLocked( moved );
			if ( moved->Directory == oldPath ) {
				moved->Directory = newPath;
				moved->DirInfo = FileInfo( newPath );
			} else {
				moved->Directory = newPath + moved->Directory.substr( oldPath.size() );
				moved->DirInfo.Filepath = moved->Directory;
			}
			mWatchesRef.emplace( moved->Directory, moved );
		}
	}
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
		WatcherInotify* source;
		WatcherInotify* destination;
		std::string filename;
	};
	// Keep the bucket array between read batches. clear() destroys the nodes but
	// does not normally release the buckets, avoiding a complete map allocation
	// on every batch while retaining average constant-time cookie lookup. A cookie
	// has one entry per source owner of the descriptor, usually exactly one.
	std::unordered_multimap<uint32_t, PendingMove> pendingMoves;
	pendingMoves.reserve( 16 );
	std::vector<BatchEvent> events;
	events.reserve( 64 );
	std::unordered_map<uint32_t, MovePair> movePairs;
	movePairs.reserve( 16 );
	// Scratch buffers reused for every event.
	std::vector<WatcherInotify*> owners;
	std::vector<WatcherInotify*> destinationOwners;
	std::vector<WatcherInotify*> partners;
	std::vector<bool> claimed;
	std::vector<PendingMove> moves;

	char* buff = new char[BUFF_SIZE];
	memset( buff, 0, BUFF_SIZE );

	// Report a source whose destination cannot be represented as one Moved event.
	// Directories require extra cleanup because every recursively watched child
	// has its own native inotify watch. Only the watches of the source's registration
	// are affected; other registrations sharing those directories keep them.
	auto emitMovedOutside = [this]( WatcherInotify* watch, const std::string& oldFileName ) {
		watch->OldFileName.clear();

		std::vector<Watcher*> eraseWatches;
		{
			Lock lock( mWatchesLock );
			const std::string movedPath( watch->Directory + oldFileName + "/" );
			for ( const auto& native : mWatches ) {
				for ( WatcherInotify* oldWatch : native.second ) {
					if ( oldWatch != watch && oldWatch->ID == watch->ID &&
						 -1 != String::strStartsWith( movedPath, oldWatch->Directory ) )
						eraseWatches.push_back( oldWatch );
				}
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
				if ( Watcher* containingWatch =
						 watcherContainsDirectory( rmWatch->Directory, watch->ID ) ) {
					handleAction( containingWatch,
								  FileSystem::fileNameFromPath( rmWatch->Directory ), IN_DELETE );
				}
			}
		}
	};

	// Same registration pairs keep the original rule: a rename inside one directory always
	// pairs, a move between directories of one tree needs the option. Different
	// registrations pair only when both opted in.
	auto canPair = []( const WatcherInotify* source, const WatcherInotify* destination ) {
		if ( source->ID == destination->ID )
			return source == destination || ( source->reportCrossDirectoryMoves &&
											  destination->reportCrossDirectoryMoves );
		return source->reportCrossDirectoryMoves && destination->reportCrossDirectoryMoves;
	};

	do {
		pollfd fd = { mFD, POLLIN, 0 };
		if ( poll( &fd, 1, 100 ) > 0 && ( fd.revents & POLLIN ) ) {
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
				// Every event is delivered to each logical owner of its descriptor.
				pendingMoves.clear();

				for ( const auto& event : events ) {
					{
						Lock lock( mWatchesLock );
						ownersLocked( event.wd, owners );
					}

					if ( event.mask & IN_MOVED_FROM ) {
						destinationOwners.clear();
						auto pair = movePairs.find( event.cookie );
						if ( pair != movePairs.end() && pair->second.destinationWd != -1 ) {
							Lock lock( mWatchesLock );
							ownersLocked( pair->second.destinationWd, destinationOwners );
						}

						// Each destination owner completes at most one source owner, so no
						// listener receives two accounts of one move. Owners of the same
						// registration are matched first.
						partners.assign( owners.size(), NULL );
						claimed.assign( destinationOwners.size(), false );
						for ( int pass = 0; pass < 2; ++pass ) {
							for ( size_t s = 0; s < owners.size(); ++s ) {
								if ( partners[s] )
									continue;
								for ( size_t d = 0; d < destinationOwners.size(); ++d ) {
									WatcherInotify* destination = destinationOwners[d];
									if ( claimed[d] ||
										 ( pass == 0 ) != ( destination->ID == owners[s]->ID ) ||
										 !canPair( owners[s], destination ) )
										continue;
									partners[s] = destination;
									claimed[d] = true;
									break;
								}
							}
						}

						for ( size_t s = 0; s < owners.size(); ++s ) {
							if ( owners[s]->Removed )
								continue;
							if ( partners[s] )
								pendingMoves.emplace( event.cookie,
													  PendingMove{ owners[s], partners[s],
																   event.filename } );
							else
								emitMovedOutside( owners[s], event.filename );
						}
					} else if ( event.mask & IN_MOVED_TO ) {
						moves.clear();
						auto range = pendingMoves.equal_range( event.cookie );
						for ( auto it = range.first; it != range.second; ++it )
							moves.push_back( std::move( it->second ) );
						pendingMoves.erase( range.first, range.second );

						for ( WatcherInotify* owner : owners ) {
							if ( owner->Removed )
								continue;
							auto move = std::find_if(
								moves.begin(), moves.end(), [owner]( const PendingMove& pending ) {
									return pending.destination == owner;
								} );
							if ( move == moves.end() || move->source->Removed ) {
								handleAction( owner, event.filename, event.mask );
								continue;
							}

							WatcherInotify* source = move->source;
							std::string sourceFilename = std::move( move->filename );
							moves.erase( move );

							if ( source == owner ) {
								owner->OldFileName = sourceFilename;
								handleAction( owner, event.filename, event.mask );
							} else if ( ( event.mask & IN_ISDIR ) && source->ID != owner->ID ) {
								// The moved subtree is still watched natively, but its watches
								// belong to the source registration and must be handed over.
								moveDirectoryBetweenRoots( source, sourceFilename, owner,
														   event.filename );
							} else {
								emitCrossDirectoryMove( source, sourceFilename, owner,
														event.filename );
							}
						}

						// Sources whose destination owner disappeared in the meantime.
						for ( PendingMove& move : moves ) {
							if ( !move.source->Removed )
								emitMovedOutside( move.source, move.filename );
						}
					} else {
						for ( WatcherInotify* owner : owners ) {
							if ( !owner->Removed )
								handleAction( owner, event.filename, event.mask );
						}
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
			// Only this registration's coverage matters: another registration watching the
			// directory does not deliver events to this one.
			Lock lock( mWatchesLock );
			found = NULL != findWatchLocked( fpath, watch->ID );
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

void FileWatcherInotify::moveDirectoryBetweenRoots( WatcherInotify* src, const std::string& srcFile,
													WatcherInotify* dst,
													const std::string& dstFile ) {
	if ( !dst->Listener || !mInitOK )
		return;

	std::string oldPath( src->Directory + srcFile );
	std::string newPath( dst->Directory + dstFile );
	FileSystem::dirAddSlashAtEnd( oldPath );
	FileSystem::dirAddSlashAtEnd( newPath );

	Lock initLock( mInitLock );
	std::vector<WatcherInotify*> sourceOwned;
	std::vector<WatcherInotify*> shared;
	{
		Lock lock( mWatchesLock );
		for ( const auto& native : mWatches ) {
			for ( WatcherInotify* watch : native.second ) {
				if ( -1 == String::strStartsWith( oldPath, watch->Directory ) )
					continue;
				if ( watch->ID == src->ID )
					sourceOwned.push_back( watch );
				else if ( watch->ID != dst->ID )
					shared.push_back( watch );
			}
		}

		// The native watches followed the directory, whichever registration owns them.
		renameWatchedDirectoriesLocked( oldPath, newPath );

		if ( dst->Recursive ) {
			// Keep the native watches so no event inside the moved tree is lost, but make
			// them report to, and be removed with, the destination registration.
			for ( WatcherInotify* watch : sourceOwned ) {
				watch->ID = dst->ID;
				watch->Listener = dst->Listener;
				watch->Recursive = true;
				watch->syntheticEvents = dst->syntheticEvents;
				watch->reportCrossDirectoryMoves = dst->reportCrossDirectoryMoves;
				watch->OldFileName.clear();
				if ( watch->Directory == newPath )
					watch->Parent = dst;
			}
		} else {
			// A non-recursive destination must not keep watching inside the moved tree.
			for ( WatcherInotify* watch : sourceOwned )
				removeWatchLocked( watch );
		}
	}

	// Install recursive coverage before notifying, like a plain Add does.
	if ( dst->Recursive ) {
		if ( sourceOwned.empty() ) {
			// The source never watched inside the directory. Adding coverage joins the native
			// watches of other registrations found there as an additional owner.
			checkForNewWatcher( dst, newPath );
		} else {
			// The handed-over tree stops where other registrations own the directories.
			std::stable_sort( shared.begin(), shared.end(),
							  []( const WatcherInotify* left, const WatcherInotify* right ) {
								  return left->Directory.size() < right->Directory.size();
							  } );
			for ( WatcherInotify* watch : shared ) {
				WatcherInotify* parent = NULL;
				{
					Lock lock( mWatchesLock );
					if ( watch->Removed || findWatchLocked( watch->Directory, dst->ID ) )
						continue;
					std::string parentPath( watch->Directory );
					FileSystem::dirRemoveSlashAtEnd( parentPath );
					parentPath = FileSystem::pathRemoveFileName( parentPath );
					FileSystem::dirAddSlashAtEnd( parentPath );
					parent = findWatchLocked( parentPath, dst->ID );
				}
				if ( parent )
					addWatch( watch->Directory, dst->Listener, true, dst->syntheticEvents,
							  dst->reportCrossDirectoryMoves, parent, true );
			}
		}
	}

	dst->Listener->handleFileAction( dst->ID, dst->Directory, dstFile, Actions::Moved,
									 src->Directory + srcFile );
}

void FileWatcherInotify::handleAction( Watcher* watch, const std::string& filename,
									   unsigned long action, const std::string& ) {
	if ( !watch || !watch->Listener || !mInitOK ) {
		return;
	}
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

		if ( !watch->OldFileName.empty() && FileSystem::isDirectory( fpath ) ) {
			/// Update the paths of the watches that moved with the directory. This is a
			/// physical move, so it applies to every registration watching inside it.
			std::string opath( watch->OldFileName );
			if ( opath[0] != FileSystem::getOSSlash() )
				opath = watch->Directory + opath;
			FileSystem::dirAddSlashAtEnd( opath );
			FileSystem::dirAddSlashAtEnd( fpath );

			Lock lock( mWatchesLock );
			renameWatchedDirectoriesLocked( opath, fpath );
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
			if ( WatcherInotify* erased = findWatchLocked( fpath, watch->ID ) )
				removeWatchLocked( erased );
		}
	}
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
