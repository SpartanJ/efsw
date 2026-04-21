#include "utest.h"
#include "test_util.hpp"

UTEST( Modified, SingleFile ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    EXPECT_TRUE( createDirectory( testDir ) );

    std::string testFile = testDir + "/test_file.txt";
    EXPECT_TRUE( createFile( testFile, "initial content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    EXPECT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );

    listener.clearEvents();

    EXPECT_TRUE( writeFile( testFile, " modified content" ) );

    listener.waitForActions( efsw::Actions::Modified, "test_file.txt" );

    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Modified, "test_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}

UTEST( Modified, MultipleWrites ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    EXPECT_TRUE( createDirectory( testDir ) );

    std::string testFile = testDir + "/test_file.txt";
    EXPECT_TRUE( createFile( testFile, "line1\n" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    EXPECT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );
    listener.clearEvents();

    for ( int i = 0; i < 3; i++ ) {
        EXPECT_TRUE( writeFile( testFile, "line" + std::to_string( i + 2 ) + "\n" ) );
        sleepMs( 50 );
    }

    listener.waitForActions( efsw::Actions::Modified, "test_file.txt" );

    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Modified, "test_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}

UTEST( Modified, AppendToFile ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    EXPECT_TRUE( createDirectory( testDir ) );

    std::string testFile = testDir + "/test_file.txt";
    EXPECT_TRUE( createFile( testFile, "start" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    EXPECT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 100 );
    listener.clearEvents();

    EXPECT_TRUE( writeFile( testFile, "_appended" ) );

    listener.waitForActions( efsw::Actions::Modified, "test_file.txt" );

    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Modified, "test_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}
