#include <efsw/DirWatcherGeneric.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/WatcherGeneric.hpp>

namespace efsw {

WatcherGeneric::WatcherGeneric( WatchID id, const std::string& directory, FileWatchListener* fwl,
								FileWatcherImpl* fw, bool recursive ) :
	Watcher( id, directory, fwl, recursive ),
	WatcherImpl( fw ),
	DirWatch( NULL ) {
	FileSystem::dirAddSlashAtEnd( Directory );

	DirWatch = new DirWatcherGeneric( NULL, this, directory, recursive, false );

	DirWatch->addChildren( false );
}

WatcherGeneric::~WatcherGeneric() {
	efSAFE_DELETE( DirWatch );
}

void WatcherGeneric::watch() {
	// Recursive scans must finish updating the complete watcher tree before callbacks are
	// delivered. A listener may immediately mutate a directory after receiving an event.
	mCollectActions = Recursive;
	if ( mCollectActions )
		mActionBatch.clear();

	DirWatch->watch();

	if ( mCollectActions ) {
		mActionBatch.dispatch( Listener );
		mCollectActions = false;
	}
}

void WatcherGeneric::watchDir( std::string dir ) {
	DirWatch->watchDir( dir );
}

bool WatcherGeneric::pathInWatches( std::string path ) {
	return DirWatch->pathInWatches( path );
}

void WatcherGeneric::moveDirectory( const std::string& directory ) {
	Directory = directory;
	FileSystem::dirAddSlashAtEnd( Directory );
	DirWatch->resetDirectory( Directory );
}

void WatcherGeneric::retire() {
	Watcher::retire();
	mActionBatch.cancel();
}

void WatcherGeneric::handleAction( const std::string& directory, const std::string& filename,
								   Action action, const std::string& oldFilename,
								   const FileInfo& ) {
	if ( Retired )
		return;
	// A kqueue fallback watch reports with its registration's ID, which addWatch() returned.
	const WatchID id = SharedActionOwner ? SharedActionOwner->ID : ID;
	if ( mCollectActions ) {
		mActionBatch.add( id, directory, filename, action, oldFilename );
	} else {
		Listener->handleFileAction( id, directory, filename, action, oldFilename );
	}
}

} // namespace efsw
