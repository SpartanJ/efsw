#include "utest.h"
#include "test_util.hpp"

UTEST( LocalVsCrossMove, CompareSameDirVsCrossDir ) {
    using namespace efsw_test;

    std::string watchedDir1 = getTemporaryDirectory() + "_dir1";
    std::string watchedDir2 = getTemporaryDirectory() + "_dir2";

    ASSERT_TRUE( createDirectory( watchedDir1 ) );
    ASSERT_TRUE( createDirectory( watchedDir2 ) );

    std::string file1 = watchedDir1 + "/file.txt";
    std::string file2 = watchedDir2 + "/file2.txt";
    ASSERT_TRUE( createFile( file1, "content" ) );
    ASSERT_TRUE( createFile( file2, "content" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId1 = fileWatcher.addWatch( watchedDir1, &listener, true );
    efsw::WatchID watchId2 = fileWatcher.addWatch( watchedDir2, &listener, true );
    ASSERT_TRUE( watchId1 > 0 );
    ASSERT_TRUE( watchId2 > 0 );

    fileWatcher.watch();
    sleepMs( 100 );

    listener.clearEvents();

    std::string fileRenamed = watchedDir1 + "/file_renamed.txt";
    ASSERT_TRUE( renameFile( file1, fileRenamed ) );

    listener.waitForEvents( 3 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Moved, "file_renamed.txt", "file.txt" ) );

    listener.clearEvents();

    std::string fileInDir2 = watchedDir2 + "/file_to_move.txt";
    ASSERT_TRUE( renameFile( fileRenamed, fileInDir2 ) );

    listener.waitForEvents( 4 );

    ASSERT_FALSE( listener.checkEvent( efsw::Actions::Moved, "file_to_move.txt", "" ) );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Delete, "file_renamed.txt" ) );
    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "file_to_move.txt" ) );

    fileWatcher.removeWatch( watchedDir1 );
    fileWatcher.removeWatch( watchedDir2 );
    removeDirectory( watchedDir1 );
    removeDirectory( watchedDir2 );
}

UTEST( NestedFolderRename, RenameFolderWithChildren ) {
    using namespace efsw_test;

    std::string testDir = getTemporaryDirectory();
    ASSERT_TRUE( createDirectory( testDir ) );

    std::string subDir = testDir + "/parent_dir";
    std::string childDir = subDir + "/child_dir";
    std::string grandchildDir = childDir + "/grandchild_dir";
    ASSERT_TRUE( createDirectory( subDir ) );
    ASSERT_TRUE( createDirectory( childDir ) );
    ASSERT_TRUE( createDirectory( grandchildDir ) );

    std::string file1 = subDir + "/file1.txt";
    std::string file2 = childDir + "/file2.txt";
    std::string file3 = grandchildDir + "/file3.txt";
    ASSERT_TRUE( createFile( file1, "content1" ) );
    ASSERT_TRUE( createFile( file2, "content2" ) );
    ASSERT_TRUE( createFile( file3, "content3" ) );

    TestListener listener;
    efsw::FileWatcher fileWatcher( false );

    efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
    ASSERT_TRUE( watchId > 0 );

    fileWatcher.watch();
    sleepMs( 100 );

    listener.clearEvents();

    std::string renamedSubDir = testDir + "/renamed_parent_dir";
    ASSERT_TRUE( renameFile( subDir, renamedSubDir ) );

    listener.waitForEvents( 2 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Moved, "renamed_parent_dir", "parent_dir" ) );

    listener.clearEvents();

    std::string renamedChildDir = renamedSubDir + "/child_dir";
    std::string renamedFile2 = renamedChildDir + "/file2.txt";
    ASSERT_TRUE( writeFile( renamedFile2, "modified_content" ) );
    sleepMs( 100 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Modified, "file2.txt" ) );

    listener.clearEvents();

    std::string newFile = renamedChildDir + "/new_child_file.txt";
    ASSERT_TRUE( createFile( newFile, "new content" ) );
    sleepMs( 100 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "new_child_file.txt" ) );

    listener.clearEvents();

    std::string newGrandchildFile = renamedSubDir + "/new_grandchild_file.txt";
    ASSERT_TRUE( createFile( newGrandchildFile, "new grandchild content" ) );
    sleepMs( 100 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Add, "new_grandchild_file.txt" ) );

    listener.clearEvents();

    std::string renamedGrandchildDir = renamedSubDir + "/renamed_child_dir";
    ASSERT_TRUE( renameFile( renamedChildDir, renamedGrandchildDir ) );
    sleepMs( 200 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Moved, "renamed_child_dir", "child_dir" ) );

    listener.clearEvents();

    std::string fileInRenamedGrandchild = renamedGrandchildDir + "/file2.txt";
    ASSERT_TRUE( writeFile( fileInRenamedGrandchild, "again_modified" ) );
    sleepMs( 100 );

    ASSERT_TRUE( listener.checkEvent( efsw::Actions::Modified, "file2.txt" ) );

    fileWatcher.removeWatch( testDir );
    removeDirectory( testDir );
}