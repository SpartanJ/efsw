#include "utest.h"
#include "test_util.hpp"
#include <filesystem>

UTEST( Symlink, FollowSymlinkToDirectory ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    std::string targetDir = testDir + "/real_target";
    std::string linkPath = testDir + "/link_to_target";

    EXPECT_TRUE( createDirectory( testDir ) );
    EXPECT_TRUE( createDirectory( targetDir ) );

    std::filesystem::create_symlink( targetDir, linkPath );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    fileWatcher.followSymlinks( true );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    EXPECT_TRUE( watchId > 0 );

    fileWatcher.watch();
    sleepMs( 100 );

    listener.clearEvents();

    std::string fileInTarget = targetDir + "/file.txt";
    EXPECT_TRUE( createFile( fileInTarget, "content" ) );
    sleepMs( 100 );

    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file.txt" ) );

    listener.clearEvents();

    EXPECT_TRUE( writeFile( fileInTarget, "modified" ) );
    sleepMs( 100 );

    EXPECT_TRUE( listener.checkEvent( efsw::Actions::Modified, "file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}

UTEST( Symlink, SymlinkTargetOutsideScope ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    std::string outsideDir = getTemporaryDirectory() + "_outside";
    std::string linkPath = testDir + "/link_to_outside";

    EXPECT_TRUE( createDirectory( testDir ) );
    EXPECT_TRUE( createDirectory( outsideDir ) );

    std::filesystem::create_symlink( outsideDir, linkPath );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    fileWatcher.followSymlinks( true );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    EXPECT_TRUE( watchId > 0 );

    fileWatcher.watch();
    sleepMs( 100 );

    listener.clearEvents();

    std::string fileInOutside = outsideDir + "/outside_file.txt";
    EXPECT_TRUE( createFile( fileInOutside, "content" ) );
    sleepMs( 100 );

    ASSERT_FALSE( listener.checkEvent( efsw::Actions::Add, "outside_file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
    removeDirectory( outsideDir );
}
