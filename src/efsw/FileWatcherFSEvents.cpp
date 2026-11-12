#include <efsw/Debug.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/FileWatcherFSEvents.hpp>
#include <efsw/String.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <sys/stat.h>
#include <sys/utsname.h>

namespace efsw {

static char DispatchQueueIdentity;

int getOSXReleaseNumber() {
	static int osxR = -1;

	if ( -1 == osxR ) {
		struct utsname os;

		if ( -1 != uname( &os ) ) {
			std::string release( os.release );

			size_t pos = release.find_first_of( '.' );

			if ( pos != std::string::npos ) {
				release = release.substr( 0, pos );
			}

			int rel = 0;

			if ( String::fromString<int>( rel, release ) ) {
				osxR = rel;
			}
		}
	}

	return osxR;
}

bool FileWatcherFSEvents::isGranular() {
	return getOSXReleaseNumber() >= 11;
}

static bool convertCFStringToStdString( CFStringRef cfString, std::string& result ) {
	// Try to get the C string pointer directly
	const char* cStr = CFStringGetCStringPtr( cfString, kCFStringEncodingUTF8 );

	if ( cStr ) {
		result.assign( cStr );
		return true;
	} else {
		// If not, manually convert it
		CFIndex length = CFStringGetLength( cfString );
		CFIndex maxSize = CFStringGetMaximumSizeForEncoding( length, kCFStringEncodingUTF8 ) +
						  1; // +1 for null terminator

		result.resize( maxSize );

		if ( CFStringGetCString( cfString, &result[0], maxSize, kCFStringEncodingUTF8 ) ) {
			result.resize( strlen( result.c_str() ) );
			return true;
		} else {
			result.clear();
			return false;
		}
	}
}

void FileWatcherFSEvents::FSEventCallback( ConstFSEventStreamRef /*streamRef*/, void* userData,
										   size_t numEvents, void* eventPaths,
										   const FSEventStreamEventFlags eventFlags[],
										   const FSEventStreamEventId eventIds[] ) {
	FileWatcherFSEvents* fileWatcher = static_cast<FileWatcherFSEvents*>( userData );
	if ( !fileWatcher->mInitOK )
		return;

	if ( fileWatcher->mEventBuffer.capacity() < numEvents )
		fileWatcher->mEventBuffer.reserve( numEvents );
	size_t eventCount = 0;

	for ( size_t i = 0; i < numEvents; i++ ) {
		if ( isGranular() ) {
			CFDictionaryRef pathInfoDict =
				static_cast<CFDictionaryRef>( CFArrayGetValueAtIndex( (CFArrayRef)eventPaths, i ) );
			CFStringRef path = static_cast<CFStringRef>(
				CFDictionaryGetValue( pathInfoDict, kFSEventStreamEventExtendedDataPathKey ) );
			CFNumberRef cfInode = static_cast<CFNumberRef>(
				CFDictionaryGetValue( pathInfoDict, kFSEventStreamEventExtendedFileIDKey ) );

			if ( cfInode ) {
				unsigned long inode = 0;
				CFNumberGetValue( cfInode, kCFNumberLongType, &inode );
				if ( eventCount == fileWatcher->mEventBuffer.size() )
					fileWatcher->mEventBuffer.emplace_back( std::string(), 0, 0 );
				FSEvent& event = fileWatcher->mEventBuffer[eventCount];
				if ( convertCFStringToStdString( path, event.Path ) ) {
					event.Flags = (long)eventFlags[i];
					event.Id = (Uint64)eventIds[i];
					event.inode = inode;
					eventCount++;
				}
			}
		} else {
			if ( eventCount == fileWatcher->mEventBuffer.size() )
				fileWatcher->mEventBuffer.emplace_back( std::string(), 0, 0 );
			FSEvent& event = fileWatcher->mEventBuffer[eventCount++];
			event.Path.assign( ( (char**)eventPaths )[i] );
			event.Flags = (long)eventFlags[i];
			event.Id = (Uint64)eventIds[i];
			event.inode = 0;
		}
	}

	std::vector<std::shared_ptr<WatcherFSEvents>>& watches = fileWatcher->mWatchBuffer;
	watches.clear();
	size_t crossMoveWatches = 0;
	{
		std::lock_guard<std::mutex> lock( fileWatcher->mWatchesMutex );
		watches.reserve( fileWatcher->mWatches.size() );
		for ( const auto& watch : fileWatcher->mWatches ) {
			watches.push_back( watch.second );
			if ( watch.second->ReportCrossDirectoryMoves )
				crossMoveWatches++;
		}
	}

	// Moves inside one registration are paired by its own processing. Only moves between
	// two registrations that both opted in need the whole callback, so skip otherwise.
	const bool correlate = isGranular() && crossMoveWatches >= 2;
	if ( correlate )
		fileWatcher->pairCrossWatchMoves( eventCount );

	for ( const auto& watcher : watches ) {
		// A listener may have removed this watch from an earlier callback of this batch.
		if ( watcher->Removed )
			continue;
		watcher->EventBuffer.clear();
		for ( size_t i = 0; i < eventCount; ++i ) {
			FSEvent& event = fileWatcher->mEventBuffer[i];
			if ( correlate && fileWatcher->mClaimedBy[i] == watcher.get() ) {
				// Deliver the destination record in place, so the Moved event keeps its
				// position among the other events of this registration.
				if ( !fileWatcher->mMovedFrom[i].empty() ) {
					watcher->EventBuffer.push_back( event );
					watcher->EventBuffer.back().MovedFrom = fileWatcher->mMovedFrom[i];
				}
				continue;
			}
			bool mustRescan = event.Flags & ( kFSEventStreamEventFlagUserDropped |
											  kFSEventStreamEventFlagKernelDropped |
											  kFSEventStreamEventFlagMustScanSubDirs );
			if ( mustRescan || watcher->handlesPath( event.Path ) )
				watcher->EventBuffer.push_back( event );
		}

		if ( !watcher->EventBuffer.empty() ) {
			watcher->handleActions( watcher->EventBuffer );
			watcher->process();
		}
	}
	watches.clear();

	efDEBUG( "\n" );
}

static const size_t noEvent = static_cast<size_t>( -1 );

const WatcherFSEvents* FileWatcherFSEvents::reportingWatch( const std::string& path ) const {
	const WatcherFSEvents* owner = NULL;
	for ( const auto& watch : mWatchBuffer ) {
		if ( !watch->Removed && watch->reportsPath( path ) &&
			 ( NULL == owner || watch->Directory.size() > owner->Directory.size() ) )
			owner = watch.get();
	}
	return owner;
}

void FileWatcherFSEvents::pairCrossWatchMoves( size_t eventCount ) {
	mClaimedBy.assign( eventCount, NULL );
	mMovedFrom.assign( eventCount, std::string() );
	mInodeNext.assign( eventCount, noEvent );
	mInodeChains.clear();

	// Chain the records of each inode in arrival order, so pairing and duplicate suppression
	// only visit records of the same file instead of the whole callback.
	for ( size_t i = 0; i < eventCount; ++i ) {
		const Uint64 inode = mEventBuffer[i].inode;
		if ( inode == 0 )
			continue;
		auto chain = mInodeChains.find( inode );
		if ( chain == mInodeChains.end() ) {
			mInodeChains.emplace( inode, InodeChain{ i, i } );
		} else {
			mInodeNext[chain->second.Last] = i;
			chain->second.Last = i;
		}
	}

	for ( size_t i = 0; i < eventCount; ++i ) {
		const FSEvent& first = mEventBuffer[i];
		if ( mClaimedBy[i] || first.inode == 0 ||
			 !( first.Flags & efswFSEventStreamEventFlagItemRenamed ) )
			continue;
		for ( size_t j = mInodeNext[i]; j != noEvent; j = mInodeNext[j] ) {
			const FSEvent& second = mEventBuffer[j];
			if ( mClaimedBy[j] || first.Path == second.Path ||
				 !( second.Flags & efswFSEventStreamEventFlagItemRenamed ) )
				continue;
			if ( pairCrossWatchMove( i, j, eventCount ) )
				break;
		}
	}
}

bool FileWatcherFSEvents::pairCrossWatchMove( size_t firstIndex, size_t secondIndex,
											  size_t eventCount ) {
	const FSEvent& first = mEventBuffer[firstIndex];
	const FSEvent& second = mEventBuffer[secondIndex];

	// Resolve ownership before touching the filesystem: most renames stay in one registration.
	const WatcherFSEvents* firstWatch = reportingWatch( first.Path );
	const WatcherFSEvents* secondWatch = reportingWatch( second.Path );
	if ( NULL == firstWatch || NULL == secondWatch || firstWatch == secondWatch ||
		 !firstWatch->ReportCrossDirectoryMoves || !secondWatch->ReportCrossDirectoryMoves )
		return false;
	// A registration reporting both paths (an outer one of nested registrations) sees the
	// whole move itself; the inner ones then get their own half as Add/Delete.
	for ( const auto& watch : mWatchBuffer ) {
		if ( watch->reportsPath( first.Path ) && watch->reportsPath( second.Path ) )
			return false;
	}

	// FSEvents does not label the old and new paths. After the rename, the missing path is
	// the source and the existing path is the destination.
	const bool firstExists = FileInfo::exists( first.Path );
	if ( firstExists == FileInfo::exists( second.Path ) )
		return false;
	const size_t sourceIndex = firstExists ? secondIndex : firstIndex;
	const size_t destinationIndex = firstExists ? firstIndex : secondIndex;
	const WatcherFSEvents* sourceWatch = firstExists ? secondWatch : firstWatch;
	const WatcherFSEvents* destinationWatch = firstExists ? firstWatch : secondWatch;
	const std::string& sourcePath = mEventBuffer[sourceIndex].Path;
	const std::string& destinationPath = mEventBuffer[destinationIndex].Path;

	// Inodes are volume-scoped, and a hard-linked file has no single identity to follow.
	struct stat sourceRootStat;
	struct stat destinationStat;
	if ( stat( sourceWatch->Directory.c_str(), &sourceRootStat ) != 0 ||
		 stat( destinationPath.c_str(), &destinationStat ) != 0 )
		return false;
	if ( static_cast<Uint64>( destinationStat.st_ino ) != first.inode ||
		 destinationStat.st_dev != sourceRootStat.st_dev ||
		 !( S_ISDIR( destinationStat.st_mode ) ||
			( S_ISREG( destinationStat.st_mode ) && destinationStat.st_nlink == 1 ) ) )
		return false;

	// Each registration skips its own records of this move, including the Created/Removed
	// duplicates FSEvents emits for the same paths, so neither reports a second account of
	// it. Other registrations covering one of the paths still see them.
	const long moveFlags = efswFSEventStreamEventFlagItemRenamed |
						   efswFSEventStreamEventFlagItemCreated |
						   efswFSEventStreamEventFlagItemRemoved;
	for ( size_t k = mInodeChains[first.inode].First; k != noEvent && k < eventCount;
		  k = mInodeNext[k] ) {
		const FSEvent& record = mEventBuffer[k];
		if ( !( record.Flags & moveFlags ) )
			continue;
		if ( record.Path == sourcePath )
			mClaimedBy[k] = sourceWatch;
		else if ( record.Path == destinationPath )
			mClaimedBy[k] = destinationWatch;
	}
	mMovedFrom[destinationIndex] = sourcePath;
	return true;
}

FileWatcherFSEvents::FileWatcherFSEvents( FileWatcher* parent ) :
	FileWatcherImpl( parent ),
	mLastWatchID( 0 ),
	mStream( NULL ),
	mDispatchQueue( NULL ),
	mWatching( false ) {
	mDispatchQueue = dispatch_queue_create( "com.efsw.fsevents", DISPATCH_QUEUE_SERIAL );
	if ( NULL != mDispatchQueue ) {
		dispatch_queue_set_specific( mDispatchQueue, &DispatchQueueIdentity, this, NULL );
		mInitOK = true;
	}
}

FileWatcherFSEvents::~FileWatcherFSEvents() {
	mInitOK = false;

	FSEventStreamRef stream = NULL;
	{
		std::lock_guard<std::mutex> lock( mWatchesMutex );
		stream = stopStream();
	}
	// Even without a stream left to stop, a callback may still be running: a listener that
	// removed the last watch stopped the stream from inside it.
	waitForCallbacks();
	releaseStreamAfterCallbacks( stream );

	{
		std::lock_guard<std::mutex> lock( mWatchesMutex );
		mWatches.clear();
	}

	if ( NULL != mDispatchQueue ) {
		dispatch_queue_set_specific( mDispatchQueue, &DispatchQueueIdentity, NULL, NULL );
		dispatch_release( mDispatchQueue );
		mDispatchQueue = NULL;
	}
}

bool FileWatcherFSEvents::rebuildStream( FSEventStreamRef& replacedStream ) {
	replacedStream = NULL;
	if ( mWatches.empty() || NULL == mDispatchQueue )
		return false;

	mStreamPaths.clear();
	mStreamPaths.reserve( mWatches.size() );
	for ( const auto& watch : mWatches )
		mStreamPaths.push_back( watch.second->DirectoryRef );

	CFArrayRef directoryArray = CFArrayCreate( kCFAllocatorDefault, mStreamPaths.data(),
											   static_cast<CFIndex>( mStreamPaths.size() ), NULL );
	mStreamPaths.clear();

	Uint32 streamFlags = kFSEventStreamCreateFlagNone;
	if ( isGranular() ) {
		streamFlags = efswFSEventStreamCreateFlagFileEvents | efswFSEventStreamCreateFlagNoDefer |
					  efswFSEventStreamCreateFlagUseExtendedData |
					  efswFSEventStreamCreateFlagUseCFTypes;
	}

	FSEventStreamContext context;
	context.version = 0;
	context.info = this;
	context.retain = NULL;
	context.release = NULL;
	context.copyDescription = NULL;

	FSEventStreamRef newStream = NULL;
	if ( NULL != directoryArray ) {
		newStream = FSEventStreamCreate( kCFAllocatorDefault, &FileWatcherFSEvents::FSEventCallback,
										 &context, directoryArray, kFSEventStreamEventIdSinceNow,
										 0., streamFlags );
	}

	if ( NULL != directoryArray )
		CFRelease( directoryArray );

	if ( NULL == newStream )
		return false;

	FSEventStreamSetDispatchQueue( newStream, mDispatchQueue );
	if ( !FSEventStreamStart( newStream ) ) {
		FSEventStreamInvalidate( newStream );
		FSEventStreamRelease( newStream );
		return false;
	}

	replacedStream = mStream;
	mStream = newStream;
	if ( NULL != replacedStream ) {
		FSEventStreamStop( replacedStream );
		FSEventStreamInvalidate( replacedStream );
	}
	return true;
}

FSEventStreamRef FileWatcherFSEvents::stopStream() {
	FSEventStreamRef stream = mStream;
	mStream = NULL;
	if ( NULL != stream ) {
		FSEventStreamStop( stream );
		FSEventStreamInvalidate( stream );
	}
	return stream;
}

void FileWatcherFSEvents::waitForCallbacks() {
	// A callback that removes a watch cannot wait for itself: the watch's Removed flag stops
	// the rest of its deliveries.
	if ( NULL != mDispatchQueue && dispatch_get_specific( &DispatchQueueIdentity ) != this )
		dispatch_sync( mDispatchQueue, ^{
					   } );
}

void FileWatcherFSEvents::releaseStreamAfterCallbacks( FSEventStreamRef stream ) {
	if ( NULL == stream )
		return;

	if ( dispatch_get_specific( &DispatchQueueIdentity ) == this ) {
		dispatch_async( mDispatchQueue, ^{
		  FSEventStreamRelease( stream );
		} );
	} else {
		dispatch_sync( mDispatchQueue, ^{
					   } );
		FSEventStreamRelease( stream );
	}
}

WatchID FileWatcherFSEvents::addWatch( const std::string& directory, FileWatchListener* watcher,
									   bool recursive, const std::vector<WatcherOption>& options ) {
	std::string dir( FileSystem::getRealPath( directory ) );

	FileInfo fi( dir );

	if ( !fi.isDirectory() ) {
		return Errors::Log::createLastError( Errors::FileNotFound, dir );
	} else if ( !fi.isReadable() ) {
		return Errors::Log::createLastError( Errors::FileNotReadable, dir );
	}

	FileSystem::dirAddSlashAtEnd( dir );

	if ( pathInWatches( dir ) ) {
		return Errors::Log::createLastError( Errors::FileRepeated, directory );
	}

	/// Check if the directory is a symbolic link
	std::string curPath;
	std::string link( FileSystem::getLinkRealPath( dir, curPath ) );

	if ( "" != link ) {
		/// If it's a symlink check if the realpath exists as a watcher, or
		/// if the path is outside the current dir
		if ( pathInWatches( link ) ) {
			return Errors::Log::createLastError( Errors::FileRepeated, directory );
		} else if ( !linkAllowed( curPath, link ) ) {
			return Errors::Log::createLastError( Errors::FileOutOfScope, dir );
		} else {
			dir = link;
		}
	}

	std::shared_ptr<WatcherFSEvents> pWatch = std::make_shared<WatcherFSEvents>();
	pWatch->Listener = watcher;
	pWatch->Directory = dir;
	pWatch->Recursive = recursive;
	pWatch->FWatcher = this;
	pWatch->ModifiedFlags =
		getOptionValue( options, Option::MacModifiedFilter, efswFSEventsModified );
	pWatch->SanitizeEvents = getOptionValue( options, Option::MacSanitizeEvents, 0 ) != 0;
	pWatch->ReportCrossDirectoryMoves =
		getOptionValue( options, Option::ReportCrossDirectoryMoves, 0 ) != 0;

	{
		std::lock_guard<std::mutex> lock( mWatchesMutex );
		pWatch->ID = ++mLastWatchID;
	}
	if ( !pWatch->init() )
		return Errors::Log::createLastError( Errors::WatcherFailed, dir );

	FSEventStreamRef replacedStream = NULL;
	{
		std::lock_guard<std::mutex> lock( mWatchesMutex );
		if ( pathInWatchesUnlocked( dir ) )
			return Errors::Log::createLastError( Errors::FileRepeated, directory );

		mWatches.insert( std::make_pair( pWatch->ID, pWatch ) );
		if ( mWatching && !rebuildStream( replacedStream ) ) {
			mWatches.erase( pWatch->ID );
			return Errors::Log::createLastError( Errors::WatcherFailed, dir );
		}
	}
	releaseStreamAfterCallbacks( replacedStream );
	return pWatch->ID;
}

void FileWatcherFSEvents::removeWatch( const std::string& directory ) {
	std::string dir( FileSystem::getRealPath( directory ) );
	FileSystem::dirAddSlashAtEnd( dir );

	FSEventStreamRef replacedStream = NULL;
	std::shared_ptr<WatcherFSEvents> removedWatch;
	{
		std::lock_guard<std::mutex> lock( mWatchesMutex );
		for ( WatchMap::iterator iter = mWatches.begin(); iter != mWatches.end(); ++iter ) {
			if ( dir == iter->second->Directory ) {
				removedWatch = iter->second;
				replacedStream = eraseWatchLocked( iter );
				break;
			}
		}
	}

	if ( removedWatch ) {
		efDEBUG( "Removed watch %s\n", removedWatch->Directory.c_str() );
		waitForCallbacks();
		releaseStreamAfterCallbacks( replacedStream );
	}
}

void FileWatcherFSEvents::removeWatch( WatchID watchid ) {
	FSEventStreamRef replacedStream = NULL;
	std::shared_ptr<WatcherFSEvents> removedWatch;
	{
		std::lock_guard<std::mutex> lock( mWatchesMutex );
		WatchMap::iterator iter = mWatches.find( watchid );
		if ( iter == mWatches.end() )
			return;

		removedWatch = iter->second;
		replacedStream = eraseWatchLocked( iter );
	}

	efDEBUG( "Removed watch %s\n", removedWatch->Directory.c_str() );
	waitForCallbacks();
	releaseStreamAfterCallbacks( replacedStream );
}

FSEventStreamRef FileWatcherFSEvents::eraseWatchLocked( WatchMap::iterator iter ) {
	iter->second->Removed = true;
	mWatches.erase( iter );
	FSEventStreamRef replacedStream = NULL;
	if ( mWatching ) {
		if ( mWatches.empty() )
			replacedStream = stopStream();
		else
			rebuildStream( replacedStream );
	}
	return replacedStream;
}

void FileWatcherFSEvents::watch() {
	FSEventStreamRef replacedStream = NULL;
	bool startFailed = false;
	{
		std::lock_guard<std::mutex> lock( mWatchesMutex );
		if ( mWatching )
			return;

		mWatching = true;
		if ( !mWatches.empty() ) {
			startFailed = !rebuildStream( replacedStream );
			if ( startFailed )
				mWatching = false;
		}
	}

	releaseStreamAfterCallbacks( replacedStream );
	if ( startFailed )
		Errors::Log::createLastError( Errors::WatcherFailed, "FSEvents" );
}

void FileWatcherFSEvents::handleAction( Watcher* /*watch*/, const std::string& /*filename*/,
										unsigned long /*action*/,
										const std::string& /*oldFilename*/ ) {
	/// Not used
}

std::vector<std::string> FileWatcherFSEvents::directories() {
	std::vector<std::string> dirs;

	std::lock_guard<std::mutex> lock( mWatchesMutex );

	dirs.reserve( mWatches.size() );

	for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
		dirs.push_back( std::string( it->second->Directory ) );
	}

	return dirs;
}

bool FileWatcherFSEvents::pathInWatches( const std::string& path ) {
	std::lock_guard<std::mutex> lock( mWatchesMutex );
	return pathInWatchesUnlocked( path );
}

bool FileWatcherFSEvents::pathInWatchesUnlocked( const std::string& path ) const {
	for ( WatchMap::const_iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
		if ( it->second->Directory == path ) {
			return true;
		}
	}

	return false;
}

} // namespace efsw

#endif
