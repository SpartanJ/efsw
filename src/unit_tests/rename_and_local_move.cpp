#include "test_util.hpp"
#include "utest.h"

using namespace efsw_test;

UTEST( LocalVsCrossMove, CompareSameDirVsCrossDir ) {
	std::string watchedDir1 = getTemporaryDirectory() + "_dir1";
	std::string watchedDir2 = getTemporaryDirectory() + "_dir2";

	EXPECT_TRUE( createDirectory( watchedDir1 ) );
	EXPECT_TRUE( createDirectory( watchedDir2 ) );

	std::string file1 = watchedDir1 + "/file.txt";
	std::string file2 = watchedDir2 + "/file2.txt";
	EXPECT_TRUE( createFile( file1, "content" ) );
	EXPECT_TRUE( createFile( file2, "content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId1 = fileWatcher.addWatch( watchedDir1, &listener, true );
	efsw::WatchID watchId2 = fileWatcher.addWatch( watchedDir2, &listener, true );
	EXPECT_TRUE( watchId1 > 0 );
	EXPECT_TRUE( watchId2 > 0 );

	std::string fileRenamed = watchedDir1 + "/file_renamed.txt";
	// Queue the complete local rename before reading inotify. Moved correlation
	// is intentionally limited to one read batch so callbacks never need to be
	// held and reordered while waiting for a potentially missing pair.
	EXPECT_TRUE( renameFile( file1, fileRenamed ) );
	fileWatcher.watch();

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Moved, "file_renamed.txt" ) );

	std::string fileInDir2 = watchedDir2 + "/file_to_move.txt";
	EXPECT_TRUE( renameFile( fileRenamed, fileInDir2 ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "file_renamed.txt" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file_to_move.txt" ) );

	ASSERT_FALSE( listener.checkEvent( efsw::Actions::Moved, "file_to_move.txt", "" ) );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Delete, "file_renamed.txt" ) );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file_to_move.txt" ) );

	fileWatcher.removeWatch( watchedDir1 );
	fileWatcher.removeWatch( watchedDir2 );
	removeDirectory( watchedDir1 );
	removeDirectory( watchedDir2 );
}

UTEST( NestedFolderRename, RenameFolderWithChildren ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	std::string subDir = testDir + "/parent_dir";
	std::string childDir = subDir + "/child_dir";
	std::string grandchildDir = childDir + "/grandchild_dir";
	EXPECT_TRUE( createDirectory( subDir ) );
	EXPECT_TRUE( createDirectory( childDir ) );
	EXPECT_TRUE( createDirectory( grandchildDir ) );

	std::string file1 = subDir + "/file1.txt";
	std::string file2 = childDir + "/file2.txt";
	std::string file3 = grandchildDir + "/file3.txt";
	EXPECT_TRUE( createFile( file1, "content1" ) );
	EXPECT_TRUE( createFile( file2, "content2" ) );
	EXPECT_TRUE( createFile( file3, "content3" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( testDir + "/watch_ready" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "watch_ready" ) );

	std::string renamedSubDir = testDir + "/renamed_parent_dir";
	EXPECT_TRUE( renameFile( subDir, renamedSubDir ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Moved, "renamed_parent_dir" ) );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Moved, "renamed_parent_dir", "parent_dir" ) );

	std::string renamedChildDir = renamedSubDir + "/child_dir";
	std::string renamedFile2 = renamedChildDir + "/file2.txt";
	EXPECT_TRUE( writeFile( renamedFile2, "modified_content" ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Modified, "file2.txt" ) );

	std::string newFile = renamedChildDir + "/new_child_file.txt";
	EXPECT_TRUE( createFile( newFile, "new content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "new_child_file.txt" ) );

	std::string newGrandchildFile = renamedSubDir + "/new_grandchild_file.txt";
	EXPECT_TRUE( createFile( newGrandchildFile, "new grandchild content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "new_grandchild_file.txt" ) );

	std::string renamedGrandchildDir = renamedSubDir + "/renamed_child_dir";
	EXPECT_TRUE( renameFile( renamedChildDir, renamedGrandchildDir ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Moved, "renamed_child_dir" ) );
	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Moved, "renamed_child_dir", "child_dir" ) );

	std::string fileInRenamedGrandchild = renamedGrandchildDir + "/new_child_file.txt";
	EXPECT_TRUE( writeFile( fileInRenamedGrandchild, "modified after rename" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Modified, "new_child_file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}
