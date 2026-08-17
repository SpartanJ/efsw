#include <efsw/Debug.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/FileWatcherFSEvents.hpp>
#include <efsw/Lock.hpp>
#include <efsw/String.hpp>
#include <efsw/System.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <sys/utsname.h>

namespace efsw {

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
	WatcherFSEvents* watcher = static_cast<WatcherFSEvents*>( userData );

	if ( watcher->EventBuffer.capacity() < numEvents )
		watcher->EventBuffer.reserve( numEvents );
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
				if ( eventCount == watcher->EventBuffer.size() )
					watcher->EventBuffer.emplace_back( std::string(), 0, 0 );
				FSEvent& event = watcher->EventBuffer[eventCount];
				if ( convertCFStringToStdString( path, event.Path ) ) {
					event.Flags = (long)eventFlags[i];
					event.Id = (Uint64)eventIds[i];
					event.inode = inode;
					eventCount++;
				}
			}
		} else {
			if ( eventCount == watcher->EventBuffer.size() )
				watcher->EventBuffer.emplace_back( std::string(), 0, 0 );
			FSEvent& event = watcher->EventBuffer[eventCount++];
			event.Path.assign( ( (char**)eventPaths )[i] );
			event.Flags = (long)eventFlags[i];
			event.Id = (Uint64)eventIds[i];
			event.inode = 0;
		}
	}

	watcher->handleActions( watcher->EventBuffer, eventCount );

	watcher->process();

	efDEBUG( "\n" );
}

FileWatcherFSEvents::FileWatcherFSEvents( FileWatcher* parent ) :
	FileWatcherImpl( parent ), mLastWatchID( 0 ) {
	mInitOK = true;

	watch();
}

FileWatcherFSEvents::~FileWatcherFSEvents() {
	mInitOK = false;

	mWatchCond.notify_all();

	WatchMap::iterator iter = mWatches.begin();

	for ( ; iter != mWatches.end(); ++iter ) {
		WatcherFSEvents* watch = iter->second;

		efSAFE_DELETE( watch );
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

	mLastWatchID++;

	WatcherFSEvents* pWatch = new WatcherFSEvents();
	pWatch->Listener = watcher;
	pWatch->ID = mLastWatchID;
	pWatch->Directory = dir;
	pWatch->Recursive = recursive;
	pWatch->FWatcher = this;
	pWatch->ModifiedFlags =
		getOptionValue( options, Option::MacModifiedFilter, efswFSEventsModified );
	pWatch->SanitizeEvents = getOptionValue( options, Option::MacSanitizeEvents, 0 ) != 0;
	pWatch->ReportCrossDirectoryMoves =
		getOptionValue( options, Option::ReportCrossDirectoryMoves, 0 ) != 0;

	pWatch->init();

	{
		Lock lock( mWatchesLock );
		mWatches.insert( std::make_pair( mLastWatchID, pWatch ) );
	}

	mWatchCond.notify_all();
	return pWatch->ID;
}

void FileWatcherFSEvents::removeWatch( const std::string& directory ) {
	std::string dir( FileSystem::getRealPath( directory ) );
	FileSystem::dirAddSlashAtEnd( dir );

	Lock lock( mWatchesLock );

	WatchMap::iterator iter = mWatches.begin();

	for ( ; iter != mWatches.end(); ++iter ) {
		if ( dir == iter->second->Directory ) {
			removeWatch( iter->second->ID );
			return;
		}
	}
}

void FileWatcherFSEvents::removeWatch( WatchID watchid ) {
	Lock lock( mWatchesLock );

	WatchMap::iterator iter = mWatches.find( watchid );

	if ( iter == mWatches.end() )
		return;

	WatcherFSEvents* watch = iter->second;

	mWatches.erase( iter );

	efDEBUG( "Removed watch %s\n", watch->Directory.c_str() );

	efSAFE_DELETE( watch );
}

void FileWatcherFSEvents::watch() {}

void FileWatcherFSEvents::handleAction( Watcher* /*watch*/, const std::string& /*filename*/,
										unsigned long /*action*/,
										const std::string& /*oldFilename*/ ) {
	/// Not used
}

std::vector<std::string> FileWatcherFSEvents::directories() {
	std::vector<std::string> dirs;

	Lock lock( mWatchesLock );

	dirs.reserve( mWatches.size() );

	for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
		dirs.push_back( std::string( it->second->Directory ) );
	}

	return dirs;
}

bool FileWatcherFSEvents::pathInWatches( const std::string& path ) {
	for ( WatchMap::iterator it = mWatches.begin(); it != mWatches.end(); ++it ) {
		if ( it->second->Directory == path ) {
			return true;
		}
	}

	return false;
}

} // namespace efsw

#endif
