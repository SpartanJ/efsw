#include "utest.h"
#include "test_util.hpp"

UTEST( Moved, RenameFile ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    ASSERT_TRUE( createDirectory( testDir ) );

    std::string oldFile = testDir + "/old_name.txt";
    std::string newFile = testDir + "/new_name.txt";
    ASSERT_TRUE( createFile( oldFile, "test content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 200 );
    listener.clearEvents();

ASSERT_TRUE( renameFile( oldFile, newFile ) );

    listener.waitForEvents( 1 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Moved, "new_name.txt", "old_name.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}

UTEST( Moved, MoveFileToSubdirectory ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    std::string subDir = testDir + "/subdir";

    ASSERT_TRUE( createDirectory( testDir ) );
    ASSERT_TRUE( createDirectory( subDir ) );

    std::string sourceFile = testDir + "/file.txt";
    std::string destFile = subDir + "/file.txt";
    ASSERT_TRUE( createFile( sourceFile, "test content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 200 );
    listener.clearEvents();

    ASSERT_TRUE( renameFile( sourceFile, destFile ) );

    listener.waitForEvents( 2 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}

UTEST( Moved, MoveFileBetweenDirectories ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    std::string dir1 = testDir + "/dir1";
    std::string dir2 = testDir + "/dir2";

    ASSERT_TRUE( createDirectory( testDir ) );
    ASSERT_TRUE( createDirectory( dir1 ) );
    ASSERT_TRUE( createDirectory( dir2 ) );

    std::string sourceFile = dir1 + "/file.txt";
    std::string destFile = dir2 + "/file.txt";
    ASSERT_TRUE( createFile( sourceFile, "test content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();

    sleepMs( 200 );
    listener.clearEvents();

    ASSERT_TRUE( renameFile( sourceFile, destFile ) );

    listener.waitForEvents( 2 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "file.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}