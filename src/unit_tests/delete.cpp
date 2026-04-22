#include "utest.h"
#include "test_util.hpp"

UTEST( Delete, SingleFile ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    EXPECT_TRUE( createDirectory( testDir ) );

    std::string testFile = testDir + "/test_file.txt";
    EXPECT_TRUE( createFile( testFile, "test content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    EXPECT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );
    listener.clearEvents();

    EXPECT_TRUE( removeFile( testFile ) );

    listener.waitForActions( efsw::Actions::Delete, "test_file.txt" );

    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Delete, "test_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}

UTEST( Delete, MultipleFiles ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    EXPECT_TRUE( createDirectory( testDir ) );

    EXPECT_TRUE( createFile( testDir + "/file1.txt", "content1" ) );
    EXPECT_TRUE( createFile( testDir + "/file2.txt", "content2" ) );
    EXPECT_TRUE( createFile( testDir + "/file3.txt", "content3" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    EXPECT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );
    listener.clearEvents();

    EXPECT_TRUE( removeFile( testDir + "/file1.txt" ) );
    EXPECT_TRUE( removeFile( testDir + "/file2.txt" ) );

    listener.waitForActions( efsw::Actions::Delete, "file1.txt" );
    listener.waitForActions( efsw::Actions::Delete, "file2.txt" );

    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Delete, "file1.txt" ) );
    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Delete, "file2.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}

UTEST( Delete, Subdirectory ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    std::string subDir = testDir + "/subdir";

    EXPECT_TRUE( createDirectory( testDir ) );
    EXPECT_TRUE( createDirectory( subDir ) );

    std::string testFile = subDir + "/nested_file.txt";
    EXPECT_TRUE( createFile( testFile, "nested content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    EXPECT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );
    listener.clearEvents();

    EXPECT_TRUE( removeFile( testFile ) );

    listener.waitForActions( efsw::Actions::Delete, "nested_file.txt" );

    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Delete, "nested_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}
