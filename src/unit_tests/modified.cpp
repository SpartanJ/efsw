#include "utest.h"
#include "test_util.hpp"

UTEST( Modified, SingleFile ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    ASSERT_TRUE( createDirectory( testDir ) );

    std::string testFile = testDir + "/test_file.txt";
    ASSERT_TRUE( createFile( testFile, "initial content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );

    listener.clearEvents();

    ASSERT_TRUE( writeFile( testFile, " modified content" ) );

    listener.waitForEvents( 1 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Modified, "test_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}

UTEST( Modified, MultipleWrites ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    ASSERT_TRUE( createDirectory( testDir ) );

    std::string testFile = testDir + "/test_file.txt";
    ASSERT_TRUE( createFile( testFile, "line1\n" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );
    listener.clearEvents();

    for ( int i = 0; i < 3; i++ ) {
        ASSERT_TRUE( writeFile( testFile, "line" + std::to_string( i + 2 ) + "\n" ) );
        sleepMs( 50 );
    }

    listener.waitForEvents( 1 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Modified, "test_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}

UTEST( Modified, AppendToFile ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    ASSERT_TRUE( createDirectory( testDir ) );

    std::string testFile = testDir + "/test_file.txt";
    ASSERT_TRUE( createFile( testFile, "start" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );
    listener.clearEvents();

    ASSERT_TRUE( writeFile( testFile, "_appended" ) );

    listener.waitForEvents( 1 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Modified, "test_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}