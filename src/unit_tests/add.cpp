#include "utest.h"
#include "test_util.hpp"

UTEST( Add, SingleFile ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    EXPECT_TRUE( createDirectory( testDir ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    EXPECT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 500 );

    std::string testFile = testDir + "/test_file.txt";
    EXPECT_TRUE( createFile( testFile, "test content" ) );

    listener.waitForActions( efsw::Actions::Add, "test_file.txt", 3000 );

    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "test_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    sleepMs( 300 );
    removeDirectory( testDir );
}

UTEST( Add, MultipleFiles ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    EXPECT_TRUE( createDirectory( testDir ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    EXPECT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 300 );

    EXPECT_TRUE( createFile( testDir + "/file1.txt", "content1" ) );
    sleepMs( 300 );
    EXPECT_TRUE( createFile( testDir + "/file2.txt", "content2" ) );
    sleepMs( 300 );
    EXPECT_TRUE( createFile( testDir + "/file3.txt", "content3" ) );

    listener.waitForActions( efsw::Actions::Add, "file1.txt", 3000 );
    listener.waitForActions( efsw::Actions::Add, "file2.txt", 3000 );
    listener.waitForActions( efsw::Actions::Add, "file3.txt", 3000 );

    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file1.txt" ) );
    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file2.txt" ) );
    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file3.txt" ) );

    fileWatcher.removeWatch( testDir );
    sleepMs( 300 );
    removeDirectory( testDir );
}

UTEST( Add, Subdirectory ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    std::string subDir = testDir + "/subdir";

    EXPECT_TRUE( createDirectory( testDir ) );
    EXPECT_TRUE( createDirectory( subDir ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    EXPECT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );

    std::string testFile = subDir + "/nested_file.txt";
    EXPECT_TRUE( createFile( testFile, "nested content" ) );

    listener.waitForActions( efsw::Actions::Add, "nested_file.txt", 3000 );

    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "nested_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    sleepMs( 300 );
    removeDirectory( testDir );
}
