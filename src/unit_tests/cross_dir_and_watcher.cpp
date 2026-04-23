#include "test_util.hpp"
#include "utest.h"

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

	listener.waitForActions( efsw::Actions::Delete, "test_file.txt" );
	listener.waitForActions( efsw::Actions::Add, "test_file.txt" );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Delete, "test_file.txt" ) );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "test_file.txt" ) );

	fileWatcher.removeWatch( watchedDir1 );
	fileWatcher.removeWatch( watchedDir2 );
	sleepMs( 300 );
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
	sleepMs( 100 );

	listener.clearEvents();

	std::string fileInUnwatched = unwatchedDir + "/test_file.txt";
	EXPECT_TRUE( renameFile( filePath, fileInUnwatched ) );

	sleepMs( 300 );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Delete, "test_file.txt" ) );

	fileWatcher.removeWatch( watchedDir );
	sleepMs( 300 );
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
	sleepMs( 100 );

	listener.clearEvents();

	std::string subDir = testDir + "/new_subdir";
	EXPECT_TRUE( createDirectory( subDir ) );

	listener.waitForActions( efsw::Actions::Add, "new_subdir" );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "new_subdir" ) );

	listener.clearEvents();

	std::string fileInSubDir = subDir + "/file_in_new_dir.txt";
	EXPECT_TRUE( createFile( fileInSubDir, "content" ) );

	listener.waitForActions( efsw::Actions::Add, "file_in_new_dir.txt" );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file_in_new_dir.txt" ) );

	fileWatcher.removeWatch( testDir );
	sleepMs( 300 );
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

	listener.waitForActions( efsw::Actions::Add, "file_before_remove.txt" );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file_before_remove.txt" ) );

	fileWatcher.removeWatch( watchId );

	sleepMs( 300 );

	listener.clearEvents();

	std::string filePath2 = testDir + "/file_after_remove.txt";
	EXPECT_TRUE( createFile( filePath2, "content" ) );

	sleepMs( 500 );

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
	sleepMs( 100 );

	listener.clearEvents();

	std::string subDirInDir2 = watchedDir2 + "/moved_folder";
	EXPECT_TRUE( renameFile( subDir, subDirInDir2 ) );

	listener.waitForActions( efsw::Actions::Delete, "moved_folder" );
	listener.waitForActions( efsw::Actions::Add, "moved_folder" );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Delete, "moved_folder" ) );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "moved_folder" ) );

	listener.clearEvents();

	std::string movedFileInDir2 = subDirInDir2 + "/child.txt";
	EXPECT_TRUE( writeFile( movedFileInDir2, "modified" ) );
	sleepMs( 100 );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Modified, "child.txt" ) );

	fileWatcher.removeWatch( watchedDir1 );
	fileWatcher.removeWatch( watchedDir2 );
	removeDirectory( watchedDir1 );
	removeDirectory( watchedDir2 );
}
