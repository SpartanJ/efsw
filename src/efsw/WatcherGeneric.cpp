#include <efsw/DirWatcherGeneric.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/WatcherGeneric.hpp>

namespace efsw {

WatcherGeneric::WatcherGeneric( WatchID id, const std::string& directory, FileWatchListener* fwl,
								FileWatcherImpl* fw, bool recursive,
								bool reportCrossDirectoryMoves ) :
	Watcher( id, directory, fwl, recursive ),
	WatcherImpl( fw ),
	DirWatch( NULL ),
	ReportCrossDirectoryMoves( reportCrossDirectoryMoves ) {
	FileSystem::dirAddSlashAtEnd( Directory );

	DirWatch = new DirWatcherGeneric( NULL, this, directory, recursive, false );

	DirWatch->addChildren( false );
}

WatcherGeneric::~WatcherGeneric() {
	efSAFE_DELETE( DirWatch );
}

void WatcherGeneric::watch() {
	// Recursive scans must finish updating the complete watcher tree before callbacks are delivered.
	// A listener may immediately mutate a directory after receiving an event.
	mCollectActions = Recursive;
	if ( mCollectActions )
		mActionBatch.clear();

	DirWatch->watch();

	if ( mCollectActions ) {
		mActionBatch.dispatch( Listener,
						   ReportCrossDirectoryMoves && FileInfo::inodeSupported() );
		mCollectActions = false;
	}
}

void WatcherGeneric::watchDir( std::string dir ) {
	if ( ReportCrossDirectoryMoves && Recursive )
		watch();
	else
		DirWatch->watchDir( dir );
}

bool WatcherGeneric::pathInWatches( std::string path ) {
	return DirWatch->pathInWatches( path );
}

void WatcherGeneric::handleAction( const std::string& directory, const std::string& filename,
								   Action action, const std::string& oldFilename,
								   const FileInfo& fileInfo ) {
	if ( mCollectActions ) {
		mActionBatch.add( ID, directory, filename, action, oldFilename, fileInfo );
	} else {
		Listener->handleFileAction( ID, directory, filename, action, oldFilename );
	}
}

} // namespace efsw
