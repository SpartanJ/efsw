#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileInfo.hpp>
#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS
#include <efsw/WatcherFSEvents.hpp>
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
	sleepMs( 100 );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( oldFile, newFile ) );

	listener.waitForActions( efsw::Actions::Moved, "new_name.txt" );

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
	sleepMs( 100 );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( sourceFile, destFile ) );

	listener.waitForActions( efsw::Actions::Add, "file.txt" );
	listener.waitForActions( efsw::Actions::Delete, "file.txt" );

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
	sleepMs( 100 );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( sourceFile, destFile ) );

	listener.waitForActions( efsw::Actions::Add, "file.txt" );
	listener.waitForActions( efsw::Actions::Delete, "file.txt" );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY || EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS
UTEST( Moved, CrossDirectoryBetweenIndependentWatchesStaysDeleteAdd ) {
	if ( useGeneric )
		return;

	std::string sourceDir = getTemporaryDirectory() + "_source";
	std::string destinationDir = getTemporaryDirectory() + "_destination";
	EXPECT_TRUE( createDirectory( sourceDir ) );
	EXPECT_TRUE( createDirectory( destinationDir ) );

	std::string sourceFile = sourceDir + "/file.txt";
	std::string destinationFile = destinationDir + "/file.txt";
	EXPECT_TRUE( createFile( sourceFile, "test content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( false, 100 );
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

	fileWatcher.watch();
	EXPECT_TRUE( createFile( enabledRoot + "/enabled_watch_ready" ) );
	EXPECT_TRUE( createFile( defaultRoot + "/default_watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "enabled_watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "default_watch_ready" ) );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( sourceFile, destinationFile ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Moved, "file.txt" ) );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Moved, "file.txt", sourceFile ) );

	fileWatcher.removeWatch( enabledRoot );
	fileWatcher.removeWatch( defaultRoot );
	removeDirectory( enabledRoot );
	removeDirectory( defaultRoot );
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
