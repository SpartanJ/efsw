#include "utest.h"
#include "test_util.hpp"

UTEST( MoveFromUnwatched, FileModificationEvents ) {
    using namespace efsw_test;

    std::string watchedDir = getTemporaryDirectory();
    std::string unwatchedDir = getTemporaryDirectory() + "_unwatched";

    ASSERT_TRUE( createDirectory( watchedDir ) );
    ASSERT_TRUE( createDirectory( unwatchedDir ) );

    std::string fileInUnwatched = unwatchedDir + "/moved_file.txt";
    ASSERT_TRUE( createFile( fileInUnwatched, "initial content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( watchedDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();
    sleepMs( 100 );

    std::string fileInWatched = watchedDir + "/moved_file.txt";
    ASSERT_TRUE( renameFile( fileInUnwatched, fileInWatched ) );

    listener.waitForEvents( 1 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "moved_file.txt" ) );

    listener.clearEvents();

    ASSERT_TRUE( writeFile( fileInWatched, " modified content" ) );
    sleepMs( 100 );

    listener.waitForEvents( 1 );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Modified, "moved_file.txt" ) );

    fileWatcher.removeWatch( watchedDir );
    removeDirectory( watchedDir );
    removeDirectory( unwatchedDir );
}

UTEST( RenameWatched, FolderChildrenAddModifiedDelete ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    std::string subDir = testDir + "/watched_subdir";

    ASSERT_TRUE( createDirectory( testDir ) );
    ASSERT_TRUE( createDirectory( subDir ) );

    std::string file1 = subDir + "/child1.txt";
    std::string file2 = subDir + "/child2.txt";
    ASSERT_TRUE( createFile( file1, "content1" ) );
    ASSERT_TRUE( createFile( file2, "content2" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();
    sleepMs( 100 );

    listener.clearEvents();

    std::string renamedSubDir = testDir + "/renamed_subdir";
    ASSERT_TRUE( renameFile( subDir, renamedSubDir ) );

    listener.waitForEvents( 1 );

    listener.clearEvents();

    std::string renamedFile1 = renamedSubDir + "/child1.txt";
    ASSERT_TRUE( writeFile( renamedFile1, " modified" ) );
    sleepMs( 200 );

    ASSERT_TRUE( listener.waitForEvents( 1, 1000 ) );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Modified, "child1.txt" ) );

    listener.clearEvents();

    std::string newFile3 = renamedSubDir + "/child3.txt";
    ASSERT_TRUE( createFile( newFile3, "content3" ) );
    sleepMs( 150 );

    listener.waitForEvents( 1 );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "child3.txt" ) );

    listener.clearEvents();

    std::string file2Path = renamedSubDir + "/child2.txt";
    ASSERT_TRUE( removeFile( file2Path ) );
    sleepMs( 150 );

    listener.waitForEvents( 1 );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Delete, "child2.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}
