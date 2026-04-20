#include "utest.h"
#include "test_util.hpp"

UTEST( CrossDirMove, FileBetweenTwoWatchedDirs ) {
    using namespace efsw_test;

    std::string watchedDir1 = getTemporaryDirectory() + "_dir1";
    std::string watchedDir2 = getTemporaryDirectory() + "_dir2";

    ASSERT_TRUE( createDirectory( watchedDir1 ) );
    ASSERT_TRUE( createDirectory( watchedDir2 ) );

    std::string filePath = watchedDir1 + "/test_file.txt";
    ASSERT_TRUE( createFile( filePath, "content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId1 = fileWatcher.addWatch( watchedDir1, &listener, true );
    efsw::WatchID watchId2 = fileWatcher.addWatch( watchedDir2, &listener, true );
    ASSERT_TRUE( watchId1 > 0 );
    ASSERT_TRUE( watchId2 > 0 );

    fileWatcher.watch();
    sleepMs( 100 );

    listener.clearEvents();

    std::string fileInDir2 = watchedDir2 + "/test_file.txt";
    ASSERT_TRUE( renameFile( filePath, fileInDir2 ) );

    listener.waitForEvents( 3 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Delete, "test_file.txt" ) );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "test_file.txt" ) );

    fileWatcher.removeWatch( watchedDir1 );
    fileWatcher.removeWatch( watchedDir2 );
    sleepMs( 300 );
    removeDirectory( watchedDir1 );
    removeDirectory( watchedDir2 );
}

UTEST( MoveOutOfWatch, FileToUnwatchedDir ) {
    using namespace efsw_test;

    std::string watchedDir = getTemporaryDirectory();
    std::string unwatchedDir = getTemporaryDirectory() + "_unwatched";

    ASSERT_TRUE( createDirectory( watchedDir ) );
    ASSERT_TRUE( createDirectory( unwatchedDir ) );

    std::string filePath = watchedDir + "/test_file.txt";
    ASSERT_TRUE( createFile( filePath, "content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( watchedDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();
    sleepMs( 100 );

    listener.clearEvents();

    std::string fileInUnwatched = unwatchedDir + "/test_file.txt";
    ASSERT_TRUE( renameFile( filePath, fileInUnwatched ) );

    sleepMs( 300 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Delete, "test_file.txt" ) );

    fileWatcher.removeWatch( watchedDir );
    sleepMs( 300 );
    removeDirectory( watchedDir );
    removeDirectory( unwatchedDir );
}

UTEST( NewDirAutoWatch, CreateDirInWatchedFolder ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    ASSERT_TRUE( createDirectory( testDir ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();
    sleepMs( 100 );

    listener.clearEvents();

    std::string subDir = testDir + "/new_subdir";
    ASSERT_TRUE( createDirectory( subDir ) );

    listener.waitForEvents( 2 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "new_subdir" ) );

    listener.clearEvents();

    std::string fileInSubDir = subDir + "/file_in_new_dir.txt";
    ASSERT_TRUE( createFile( fileInSubDir, "content" ) );

    listener.waitForEvents( 1 );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "file_in_new_dir.txt" ) );

    fileWatcher.removeWatch( testDir );
    sleepMs( 300 );
    removeDirectory( testDir );
}

UTEST( RemoveWatch, StopEventsAfterRemoval ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    ASSERT_TRUE( createDirectory( testDir ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();
    sleepMs( 100 );

    listener.clearEvents();

    std::string filePath = testDir + "/file_before_remove.txt";
    ASSERT_TRUE( createFile( filePath, "content" ) );

    listener.waitForEvents( 1 );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "file_before_remove.txt" ) );

    fileWatcher.removeWatch( watchId );

    sleepMs( 300 );

    listener.clearEvents();

    std::string filePath2 = testDir + "/file_after_remove.txt";
    ASSERT_TRUE( createFile( filePath2, "content" ) );

    sleepMs( 500 );

    ASSERT_EQ( 0, static_cast<int>( listener.getEventCount() ) );

    removeDirectory( testDir );
}

UTEST( MoveFolderCrossDir, FolderBetweenTwoWatchedDirs ) {
    using namespace efsw_test;

    std::string watchedDir1 = getTemporaryDirectory() + "_dir1";
    std::string watchedDir2 = getTemporaryDirectory() + "_dir2";

    ASSERT_TRUE( createDirectory( watchedDir1 ) );
    ASSERT_TRUE( createDirectory( watchedDir2 ) );

    std::string subDir = watchedDir1 + "/moved_folder";
    std::string fileInSubDir = subDir + "/child.txt";
    ASSERT_TRUE( createDirectory( subDir ) );
    ASSERT_TRUE( createFile( fileInSubDir, "content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId1 = fileWatcher.addWatch( watchedDir1, &listener, true );
    efsw::WatchID watchId2 = fileWatcher.addWatch( watchedDir2, &listener, true );
    ASSERT_TRUE( watchId1 > 0 );
    ASSERT_TRUE( watchId2 > 0 );

    fileWatcher.watch();
    sleepMs( 100 );

    listener.clearEvents();

    std::string subDirInDir2 = watchedDir2 + "/moved_folder";
    ASSERT_TRUE( renameFile( subDir, subDirInDir2 ) );

    listener.waitForEvents( 3 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Delete, "moved_folder" ) );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "moved_folder" ) );

    listener.clearEvents();

    std::string movedFileInDir2 = subDirInDir2 + "/child.txt";
    ASSERT_TRUE( writeFile( movedFileInDir2, "modified" ) );
    sleepMs( 100 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Modified, "child.txt" ) );

    fileWatcher.removeWatch( watchedDir1 );
    fileWatcher.removeWatch( watchedDir2 );
    removeDirectory( watchedDir1 );
    removeDirectory( watchedDir2 );
}