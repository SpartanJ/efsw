#include "utest.h"
#include "test_util.hpp"

UTEST( Add, SingleFile ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    ASSERT_TRUE( createDirectory( testDir ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );

    std::string testFile = testDir + "/test_file.txt";
    ASSERT_TRUE( createFile( testFile, "test content" ) );

    listener.waitForEvents( 1 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "test_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    sleepMs( 300 );
    removeDirectory( testDir );
}

UTEST( Add, MultipleFiles ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    ASSERT_TRUE( createDirectory( testDir ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 300 );

    ASSERT_TRUE( createFile( testDir + "/file1.txt", "content1" ) );
    sleepMs( 300 );
    ASSERT_TRUE( createFile( testDir + "/file2.txt", "content2" ) );
    sleepMs( 300 );
    ASSERT_TRUE( createFile( testDir + "/file3.txt", "content3" ) );

    sleepMs( 500 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "file1.txt" ) );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "file2.txt" ) );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "file3.txt" ) );

    fileWatcher.removeWatch( testDir );
    sleepMs( 300 );
    removeDirectory( testDir );
}

UTEST( Add, Subdirectory ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    std::string subDir = testDir + "/subdir";

    ASSERT_TRUE( createDirectory( testDir ) );
    ASSERT_TRUE( createDirectory( subDir ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );

    std::string testFile = subDir + "/nested_file.txt";
    ASSERT_TRUE( createFile( testFile, "nested content" ) );

    listener.waitForEvents( 1 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "nested_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    sleepMs( 300 );
    removeDirectory( testDir );
}
