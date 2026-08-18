#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileSystem.hpp>

using namespace efsw_test;

UTEST( CrossDirMove, FileBetweenTwoWatchedDirs ) {
	std::string watchedDir1 = getTemporaryDirectory() + "_dir1";
	std::string watchedDir2 = getTemporaryDirectory() + "_dir2";

	EXPECT_TRUE( createDirectory( watchedDir1 ) );
	EXPECT_TRUE( createDirectory( watchedDir2 ) );

	std::string filePath = watchedDir1 + "/test_file.txt";
	EXPECT_TRUE( createFile( filePath, "content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId1 = fileWatcher.addWatch( watchedDir1, &listener, true );
	efsw::WatchID watchId2 = fileWatcher.addWatch( watchedDir2, &listener, true );
	EXPECT_TRUE( watchId1 > 0 );
	EXPECT_TRUE( watchId2 > 0 );

	fileWatcher.watch();
	sleepMs( 100 );
	listener.clearEvents();

	std::string fileInDir2 = watchedDir2 + "/test_file.txt";
	EXPECT_TRUE( renameFile( filePath, fileInDir2 ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "test_file.txt" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "test_file.txt" ) );

	fileWatcher.removeWatch( watchedDir1 );
	fileWatcher.removeWatch( watchedDir2 );
	removeDirectory( watchedDir1 );
	removeDirectory( watchedDir2 );
}

UTEST( MoveOutOfWatch, FileToUnwatchedDir ) {
	std::string watchedDir = getTemporaryDirectory();
	std::string unwatchedDir = getTemporaryDirectory() + "_unwatched";

	EXPECT_TRUE( createDirectory( watchedDir ) );
	EXPECT_TRUE( createDirectory( unwatchedDir ) );

	std::string filePath = watchedDir + "/test_file.txt";
	EXPECT_TRUE( createFile( filePath, "content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( watchedDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( watchedDir + "/watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready" ) );
	listener.clearEvents();

	std::string fileInUnwatched = unwatchedDir + "/test_file.txt";
	EXPECT_TRUE( renameFile( filePath, fileInUnwatched ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "test_file.txt" ) );

	fileWatcher.removeWatch( watchedDir );
	removeDirectory( watchedDir );
	removeDirectory( unwatchedDir );
}

UTEST( NewDirAutoWatch, CreateDirInWatchedFolder ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( testDir + "/watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready" ) );
	listener.clearEvents();

	std::string subDir = testDir + "/new_subdir";
	EXPECT_TRUE( createDirectory( subDir ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "new_subdir" ) );

	listener.clearEvents();

	std::string fileInSubDir = subDir + "/file_in_new_dir.txt";
	EXPECT_TRUE( createFile( fileInSubDir, "content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file_in_new_dir.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( RemoveWatch, StopEventsAfterRemoval ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, false );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	sleepMs( 100 );
	listener.clearEvents();

	std::string filePath = testDir + "/file_before_remove.txt";
	EXPECT_TRUE( createFile( filePath, "content" ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file_before_remove.txt" ) );

	fileWatcher.removeWatch( watchId );

	sleepMs( 100 );

	listener.clearEvents();

	std::string filePath2 = testDir + "/file_after_remove.txt";
	EXPECT_TRUE( createFile( filePath2, "content" ) );

	sleepMs( 100 );

	ASSERT_EQ( 0, static_cast<int>( listener.getEventCount() ) );

	removeDirectory( testDir );
}

UTEST( MoveFolderCrossDir, FolderBetweenTwoWatchedDirs ) {
	std::string watchedDir1 = getTemporaryDirectory() + "_dir1";
	std::string watchedDir2 = getTemporaryDirectory() + "_dir2";

	EXPECT_TRUE( createDirectory( watchedDir1 ) );
	EXPECT_TRUE( createDirectory( watchedDir2 ) );

	std::string subDir = watchedDir1 + "/moved_folder";
	std::string fileInSubDir = subDir + "/child.txt";
	EXPECT_TRUE( createDirectory( subDir ) );
	EXPECT_TRUE( createFile( fileInSubDir, "content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId1 = fileWatcher.addWatch( watchedDir1, &listener, true );
	efsw::WatchID watchId2 = fileWatcher.addWatch( watchedDir2, &listener, true );
	EXPECT_TRUE( watchId1 > 0 );
	EXPECT_TRUE( watchId2 > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( watchedDir1 + "/watch_ready_1" ) );
	EXPECT_TRUE( createFile( watchedDir2 + "/watch_ready_2" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready_1" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready_2" ) );
	listener.clearEvents();

	std::string subDirInDir2 = watchedDir2 + "/moved_folder";
	EXPECT_TRUE( renameFile( subDir, subDirInDir2 ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "moved_folder" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "moved_folder" ) );

	listener.clearEvents();

	std::string movedFileInDir2 = subDirInDir2 + "/child.txt";
	EXPECT_TRUE( writeFile( movedFileInDir2, "modified" ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Modified, "child.txt" ) );

	fileWatcher.removeWatch( watchedDir1 );
	fileWatcher.removeWatch( watchedDir2 );
	removeDirectory( watchedDir1 );
	removeDirectory( watchedDir2 );
}

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY || EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS || \
	EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE || EFSW_PLATFORM == EFSW_PLATFORM_WIN32
// With ReportCrossDirectoryMoves enabled, a rename across subdirectories of a single recursive
// watch should produce exactly one Moved event (no Delete, no Add).
UTEST( CrossDirMove, ReportsMovedEventWithOptionRecursive ) {
	std::string rootDir = getTemporaryDirectory();
	std::string tmpDir = rootDir + "/tmp";
	std::string dataDir = rootDir + "/data";

	EXPECT_TRUE( createDirectory( rootDir ) );
	EXPECT_TRUE( createDirectory( tmpDir ) );
	EXPECT_TRUE( createDirectory( dataDir ) );

	std::string srcFile = tmpDir + "/upload.tmp";
	EXPECT_TRUE( createFile( srcFile, "content" ) );
	std::string canonicalSrcFile = efsw::FileSystem::getRealPath( srcFile );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	std::vector<efsw::WatcherOption> options = { { efsw::Options::ReportCrossDirectoryMoves, 1 } };
	efsw::WatchID watchId = fileWatcher.addWatch( rootDir, &listener, true, options );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( rootDir + "/watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready" ) );
	listener.clearEvents();

	std::string dstFile = dataDir + "/config.json";
	EXPECT_TRUE( renameFile( srcFile, dstFile ) );

	// Expect a single Moved event for the destination filename
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Moved, "config.json" ) );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Moved, "config.json", canonicalSrcFile ) );

	// There must be no Delete or Add events for these filenames
	EXPECT_FALSE( listener.checkEvent( efsw::Actions::Delete, "upload.tmp" ) );
	EXPECT_FALSE( listener.checkEvent( efsw::Actions::Add, "config.json" ) );

	fileWatcher.removeWatch( rootDir );
	removeDirectory( rootDir );
}
#endif

// Without the option, cross-dir moves across a recursive watch still produce Delete+Add.
UTEST( CrossDirMove, FallsBackToDeleteAddWithoutOption ) {
	std::string rootDir = getTemporaryDirectory();
	std::string tmpDir = rootDir + "/tmp";
	std::string dataDir = rootDir + "/data";

	EXPECT_TRUE( createDirectory( rootDir ) );
	EXPECT_TRUE( createDirectory( tmpDir ) );
	EXPECT_TRUE( createDirectory( dataDir ) );

	std::string srcFile = tmpDir + "/upload.tmp";
	EXPECT_TRUE( createFile( srcFile, "content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( rootDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( rootDir + "/watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready" ) );
	listener.clearEvents();

	std::string dstFile = dataDir + "/config.json";
	EXPECT_TRUE( renameFile( srcFile, dstFile ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "upload.tmp" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "config.json" ) );
	EXPECT_FALSE( listener.checkEvent( efsw::Actions::Moved, "config.json" ) );

	fileWatcher.removeWatch( rootDir );
	removeDirectory( rootDir );
}

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS
UTEST( FSEvents, RebuildsSharedStreamAfterWatchingStarts ) {
	if ( useGeneric )
		return;

	std::string rootDir = getTemporaryDirectory();
	std::string watchedDir1 = rootDir + "/watch_1";
	std::string watchedDir2 = rootDir + "/watch_2";
	ASSERT_TRUE( createDirectory( rootDir ) );
	ASSERT_TRUE( createDirectory( watchedDir1 ) );
	ASSERT_TRUE( createDirectory( watchedDir2 ) );

	TestListener listener;
	{
		efsw::FileWatcher fileWatcher;
		efsw::WatchID watchId1 = fileWatcher.addWatch( watchedDir1, &listener, false );
		ASSERT_TRUE( watchId1 > 0 );
		fileWatcher.watch();

		ASSERT_TRUE( createFile( watchedDir1 + "/initial_watch.txt", "content" ) );
		ASSERT_TRUE( listener.waitForActions( efsw::Actions::Add, "initial_watch.txt" ) );
		listener.clearEvents();

		efsw::WatchID watchId2 = fileWatcher.addWatch( watchedDir2, &listener, false );
		ASSERT_TRUE( watchId2 > 0 );
		ASSERT_TRUE( createFile( watchedDir1 + "/after_add_1.txt", "content" ) );
		ASSERT_TRUE( createFile( watchedDir2 + "/after_add_2.txt", "content" ) );
		ASSERT_TRUE( listener.waitForActions( efsw::Actions::Add, "after_add_1.txt" ) );
		ASSERT_TRUE( listener.waitForActions( efsw::Actions::Add, "after_add_2.txt" ) );

		fileWatcher.removeWatch( watchId1 );
		listener.clearEvents();
		ASSERT_TRUE( createFile( watchedDir1 + "/after_remove_1.txt", "content" ) );
		ASSERT_TRUE( createFile( watchedDir2 + "/after_remove_2.txt", "content" ) );
		ASSERT_TRUE( listener.waitForActions( efsw::Actions::Add, "after_remove_2.txt" ) );
		EXPECT_FALSE( listener.checkEvent( efsw::Actions::Add, "after_remove_1.txt" ) );
	}

	removeDirectory( rootDir );
}

// FSEvents limits the system to 1024 stream clients. A FileWatcher with more logical watches must
// continue receiving events because all its paths share one FSEventStreamRef.
UTEST( FSEvents, MoreThanSystemClientLimitSharesStream ) {
	if ( useGeneric )
		return;

	static const int WatchCount = 1100;
	std::string rootDir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( rootDir ) );

	TestListener listener;
	{
		efsw::FileWatcher fileWatcher;
		std::string lastDirectory;
		for ( int i = 0; i < WatchCount; ++i ) {
			lastDirectory = rootDir + "/watch_" + std::to_string( i );
			ASSERT_TRUE( createDirectory( lastDirectory ) );
			ASSERT_TRUE( fileWatcher.addWatch( lastDirectory, &listener, false ) > 0 );
		}

		fileWatcher.watch();
		ASSERT_TRUE( createFile( lastDirectory + "/after_client_limit.txt", "content" ) );
		EXPECT_TRUE(
			listener.waitForActions( efsw::Actions::Add, "after_client_limit.txt", 5000 ) );
	}

	removeDirectory( rootDir );
}
#endif
