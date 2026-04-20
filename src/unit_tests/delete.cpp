#include "utest.h"
#include "test_util.hpp"

UTEST( Delete, SingleFile ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    ASSERT_TRUE( createDirectory( testDir ) );

    std::string testFile = testDir + "/test_file.txt";
    ASSERT_TRUE( createFile( testFile, "test content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );
    listener.clearEvents();

    ASSERT_TRUE( removeFile( testFile ) );

    listener.waitForEvents( 1 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Delete, "test_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}

UTEST( Delete, MultipleFiles ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    ASSERT_TRUE( createDirectory( testDir ) );

    ASSERT_TRUE( createFile( testDir + "/file1.txt", "content1" ) );
    ASSERT_TRUE( createFile( testDir + "/file2.txt", "content2" ) );
    ASSERT_TRUE( createFile( testDir + "/file3.txt", "content3" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );
    listener.clearEvents();

    ASSERT_TRUE( removeFile( testDir + "/file1.txt" ) );
    ASSERT_TRUE( removeFile( testDir + "/file2.txt" ) );

    listener.waitForEvents( 2 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Delete, "file1.txt" ) );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Delete, "file2.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}

UTEST( Delete, Subdirectory ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    std::string subDir = testDir + "/subdir";

    ASSERT_TRUE( createDirectory( testDir ) );
    ASSERT_TRUE( createDirectory( subDir ) );

    std::string testFile = subDir + "/nested_file.txt";
    ASSERT_TRUE( createFile( testFile, "nested content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );
    listener.clearEvents();

    ASSERT_TRUE( removeFile( testFile ) );

    listener.waitForEvents( 1 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Delete, "nested_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}