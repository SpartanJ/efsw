#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>
#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS
#include <efsw/WatcherFSEvents.hpp>
#endif
#ifdef EFSW_PLATFORM_POSIX
#include <unistd.h>
#endif

using namespace efsw_test;

UTEST( Moved, RenameFile ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	std::string oldFile = testDir + "/old_name.txt";
	std::string newFile = testDir + "/new_name.txt";
	EXPECT_TRUE( createFile( oldFile, "test content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( testDir + "/watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready" ) );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( oldFile, newFile ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Moved, "new_name.txt" ) );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Moved, "new_name.txt", "old_name.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Moved, MoveFileToSubdirectory ) {
	std::string testDir = getTemporaryDirectory();
	std::string subDir = testDir + "/subdir";

	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( subDir ) );

	std::string sourceFile = testDir + "/file.txt";
	std::string destFile = subDir + "/file.txt";
	EXPECT_TRUE( createFile( sourceFile, "test content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( testDir + "/watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready" ) );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( sourceFile, destFile ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file.txt" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "file.txt" ) );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Moved, MoveFileBetweenDirectories ) {
	std::string testDir = getTemporaryDirectory();
	std::string dir1 = testDir + "/dir1";
	std::string dir2 = testDir + "/dir2";

	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( dir1 ) );
	EXPECT_TRUE( createDirectory( dir2 ) );

	std::string sourceFile = dir1 + "/file.txt";
	std::string destFile = dir2 + "/file.txt";
	EXPECT_TRUE( createFile( sourceFile, "test content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( testDir + "/watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready" ) );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( sourceFile, destFile ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file.txt" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "file.txt" ) );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY || EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS || \
	EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE || EFSW_PLATFORM == EFSW_PLATFORM_WIN32
UTEST( Moved, CrossDirectoryBetweenIndependentWatchesStaysDeleteAdd ) {
	std::string sourceDir = getTemporaryDirectory() + "_source";
	std::string destinationDir = getTemporaryDirectory() + "_destination";
	EXPECT_TRUE( createDirectory( sourceDir ) );
	EXPECT_TRUE( createDirectory( destinationDir ) );

	std::string sourceFile = sourceDir + "/file.txt";
	std::string destinationFile = destinationDir + "/file.txt";
	EXPECT_TRUE( createFile( sourceFile, "test content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );
	std::vector<efsw::WatcherOption> options = { { efsw::Options::ReportCrossDirectoryMoves, 1 } };
	EXPECT_TRUE( fileWatcher.addWatch( sourceDir, &listener, true, options ) > 0 );
	EXPECT_TRUE( fileWatcher.addWatch( destinationDir, &listener, true, options ) > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( sourceDir + "/source_watch_ready" ) );
	EXPECT_TRUE( createFile( destinationDir + "/destination_watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "source_watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "destination_watch_ready" ) );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( sourceFile, destinationFile ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "file.txt" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file.txt" ) );
	EXPECT_FALSE( listener.checkEvent( efsw::Actions::Moved, "file.txt", sourceFile ) );

	fileWatcher.removeWatch( sourceDir );
	fileWatcher.removeWatch( destinationDir );
	removeDirectory( sourceDir );
	removeDirectory( destinationDir );
}

UTEST( Moved, CrossDirectoryDirectoryKeepsSubtreeWatched ) {
	std::string testDir = getTemporaryDirectory();
	std::string sourceDir = testDir + "/source";
	std::string destinationDir = testDir + "/destination";
	std::string movedDir = sourceDir + "/moved_dir";
	std::string destinationMovedDir = destinationDir + "/moved_dir";
	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( sourceDir ) );
	EXPECT_TRUE( createDirectory( destinationDir ) );
	EXPECT_TRUE( createDirectory( movedDir ) );
	EXPECT_TRUE( createFile( movedDir + "/child.txt", "content" ) );
	std::string canonicalSource = efsw::FileSystem::getRealPath( movedDir );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );
	std::vector<efsw::WatcherOption> options = { { efsw::Options::ReportCrossDirectoryMoves, 1 } };
	EXPECT_TRUE( fileWatcher.addWatch( testDir, &listener, true, options ) > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( testDir + "/watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready" ) );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( movedDir, destinationMovedDir ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Moved, "moved_dir" ) );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Moved, "moved_dir", canonicalSource ) );
	EXPECT_FALSE( listener.checkEvent( efsw::Actions::Delete, "moved_dir" ) );
	EXPECT_FALSE( listener.checkEvent( efsw::Actions::Add, "moved_dir" ) );

	listener.clearEvents();
	EXPECT_TRUE( writeFile( destinationMovedDir + "/child.txt", "modified" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Modified, "child.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}
#endif

#ifdef EFSW_PLATFORM_POSIX
UTEST( Moved, GenericHardLinkCreationIsAdd ) {
	if ( !useGeneric )
		return;

	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );
	std::string sourceFile = testDir + "/z_original.txt";
	std::string newLink = testDir + "/a_link.txt";
	EXPECT_TRUE( createFile( sourceFile, "content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( true, 100 );
	std::vector<efsw::WatcherOption> options = { { efsw::Options::ReportCrossDirectoryMoves, 1 } };
	EXPECT_TRUE( fileWatcher.addWatch( testDir, &listener, true, options ) > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( testDir + "/watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready" ) );
	listener.clearEvents();

	EXPECT_EQ( 0, link( sourceFile.c_str(), newLink.c_str() ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "a_link.txt" ) );
	EXPECT_FALSE( listener.checkEvent( efsw::Actions::Moved, "a_link.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Moved, GenericHardLinkRenameStaysDeleteAdd ) {
	if ( !useGeneric )
		return;

	std::string testDir = getTemporaryDirectory();
	std::string sourceDir = testDir + "/source";
	std::string destinationDir = testDir + "/destination";
	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( sourceDir ) );
	EXPECT_TRUE( createDirectory( destinationDir ) );

	std::string sourceFile = sourceDir + "/old_name.txt";
	std::string otherLink = sourceDir + "/other_link.txt";
	std::string destinationFile = destinationDir + "/new_name.txt";
	EXPECT_TRUE( createFile( sourceFile, "content" ) );
	EXPECT_EQ( 0, link( sourceFile.c_str(), otherLink.c_str() ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( true, 100 );
	std::vector<efsw::WatcherOption> options = { { efsw::Options::ReportCrossDirectoryMoves, 1 } };
	EXPECT_TRUE( fileWatcher.addWatch( testDir, &listener, true, options ) > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( testDir + "/watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready" ) );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( sourceFile, destinationFile ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "old_name.txt" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "new_name.txt" ) );
	EXPECT_FALSE( listener.checkEvent( efsw::Actions::Moved, "new_name.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}
#endif

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY
UTEST( Moved, CrossDirectoryOptionIsOwnedByRecursiveWatch ) {
	if ( useGeneric )
		return;

	std::string enabledRoot = getTemporaryDirectory() + "_enabled";
	std::string sourceDir = enabledRoot + "/source";
	std::string destinationDir = enabledRoot + "/destination";
	std::string defaultRoot = getTemporaryDirectory() + "_default";
	EXPECT_TRUE( createDirectory( enabledRoot ) );
	EXPECT_TRUE( createDirectory( sourceDir ) );
	EXPECT_TRUE( createDirectory( destinationDir ) );
	EXPECT_TRUE( createDirectory( defaultRoot ) );

	std::string sourceFile = sourceDir + "/file.txt";
	std::string destinationFile = destinationDir + "/file.txt";
	EXPECT_TRUE( createFile( sourceFile, "test content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( false, 100 );
	std::vector<efsw::WatcherOption> options = { { efsw::Options::ReportCrossDirectoryMoves, 1 } };
	EXPECT_TRUE( fileWatcher.addWatch( enabledRoot, &listener, true, options ) > 0 );
	// Adding a watch with the default options must not disable the first watch's option.
	EXPECT_TRUE( fileWatcher.addWatch( defaultRoot, &listener, true ) > 0 );

	// Queue the complete rename before reading inotify. This test verifies
	// option ownership, not the documented best-effort behavior when a move
	// pair is split across read batches.
	EXPECT_TRUE( renameFile( sourceFile, destinationFile ) );
	fileWatcher.watch();
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Moved, "file.txt" ) );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Moved, "file.txt", sourceFile ) );

	fileWatcher.removeWatch( enabledRoot );
	fileWatcher.removeWatch( defaultRoot );
	removeDirectory( enabledRoot );
	removeDirectory( defaultRoot );
}

UTEST( Moved, ConcurrentCrossDirectoryMovesKeepTheirCookies ) {
	if ( useGeneric )
		return;

	const int fileCount = 32;
	std::string root = getTemporaryDirectory();
	std::string sourceDir = root + "/source";
	std::string destinationDir = root + "/destination";
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( sourceDir ) );
	EXPECT_TRUE( createDirectory( destinationDir ) );

	for ( int i = 0; i < fileCount; ++i )
		EXPECT_TRUE( createFile( sourceDir + "/file_" + std::to_string( i ), "content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( false, 100 );
	std::vector<efsw::WatcherOption> options = { { efsw::Options::ReportCrossDirectoryMoves, 1 } };
	EXPECT_TRUE( fileWatcher.addWatch( root, &listener, true, options ) > 0 );

	// Queue the burst before the reader starts so all completed rename pairs
	// fit in one inotify read. This deterministically exercises interleaved
	// cookie correlation without asserting cross-read pairing.
	for ( int i = 0; i < fileCount; ++i ) {
		const std::string filename = "file_" + std::to_string( i );
		EXPECT_TRUE( renameFile( sourceDir + "/" + filename, destinationDir + "/" + filename ) );
	}
	fileWatcher.watch();

	for ( int i = 0; i < fileCount; ++i ) {
		const std::string filename = "file_" + std::to_string( i );
		EXPECT_TRUE( listener.waitForActions( efsw::Actions::Moved, filename ) );
		EXPECT_TRUE(
			listener.checkEvent( efsw::Actions::Moved, filename, sourceDir + "/" + filename ) );
	}

	fileWatcher.removeWatch( root );
	removeDirectory( root );
}

UTEST( Moved, DeleteAddFallbackPreservesInotifyQueueOrder ) {
	if ( useGeneric )
		return;

	std::string root = getTemporaryDirectory();
	std::string sourceDir = root + "/source";
	std::string destinationDir = root + "/destination";
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( sourceDir ) );
	EXPECT_TRUE( createDirectory( destinationDir ) );
	EXPECT_TRUE( createFile( sourceDir + "/file.txt", "content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( false, 100 );
	EXPECT_TRUE( fileWatcher.addWatch( sourceDir, &listener, false ) > 0 );
	EXPECT_TRUE( fileWatcher.addWatch( destinationDir, &listener, false ) > 0 );

	// Queue the complete operation before reading so FROM and TO are replayed
	// from one batch. Independent logical watches intentionally use Delete + Add;
	// those callbacks must retain the source-before-destination kernel order.
	EXPECT_TRUE( renameFile( sourceDir + "/file.txt", destinationDir + "/file.txt" ) );
	fileWatcher.watch();
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "file.txt" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file.txt" ) );

	const auto events = listener.getEvents();
	size_t deletePosition = events.size();
	size_t addPosition = events.size();
	for ( size_t i = 0; i < events.size(); ++i ) {
		if ( std::get<1>( events[i] ) != "file.txt" )
			continue;
		if ( std::get<0>( events[i] ) == efsw::Actions::Delete )
			deletePosition = i;
		else if ( std::get<0>( events[i] ) == efsw::Actions::Add )
			addPosition = i;
	}
	EXPECT_TRUE( deletePosition < addPosition );

	fileWatcher.removeWatch( sourceDir );
	fileWatcher.removeWatch( destinationDir );
	removeDirectory( root );
}
#endif

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS
UTEST( Moved, FSEventsCrossDirectoryInsideRecursiveWatchWithOption ) {
	std::string testDir = getTemporaryDirectory();
	std::string sourceDir = testDir + "/source";
	std::string destinationDir = testDir + "/destination";
	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( sourceDir ) );
	EXPECT_TRUE( createDirectory( destinationDir ) );

	std::string sourceFile = sourceDir + "/old_name.txt";
	std::string destinationFile = destinationDir + "/new_name.txt";
	EXPECT_TRUE( createFile( sourceFile, "test content" ) );
	efsw::FileInfo sourceInfo( sourceFile );
	EXPECT_TRUE( renameFile( sourceFile, destinationFile ) );

	TestListener listener;
	efsw::WatcherFSEvents watcher;
	watcher.ID = 1;
	watcher.Directory = testDir + "/";
	watcher.Listener = &listener;
	watcher.Recursive = true;
	watcher.ReportCrossDirectoryMoves = true;

	std::vector<efsw::FSEvent> events = {
		efsw::FSEvent( sourceFile, efsw::efswFSEventStreamEventFlagItemRenamed, 1,
					   sourceInfo.Inode ),
		efsw::FSEvent( destinationFile, efsw::efswFSEventStreamEventFlagItemRenamed, 2,
					   sourceInfo.Inode ) };
	watcher.handleActions( events );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Moved, "new_name.txt", sourceFile ) );
	EXPECT_FALSE( listener.checkEvent( efsw::Actions::Delete, "old_name.txt" ) );
	EXPECT_FALSE( listener.checkEvent( efsw::Actions::Add, "new_name.txt" ) );

	removeDirectory( testDir );
}
#endif
