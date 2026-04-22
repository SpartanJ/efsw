#include "test_util.hpp"
#include "utest.h"

using namespace efsw_test;

UTEST( MoveFromUnwatched, FileModificationEvents ) {
	for ( auto useGeneric : fileWatchers ) {
		std::string watchedDir = getTemporaryDirectory();
		std::string unwatchedDir = getTemporaryDirectory() + "_unwatched";

		EXPECT_TRUE( createDirectory( watchedDir ) );
		EXPECT_TRUE( createDirectory( unwatchedDir ) );

		std::string fileInUnwatched = unwatchedDir + "/moved_file.txt";
		EXPECT_TRUE( createFile( fileInUnwatched, "initial content" ) );

		TestListener listener;
		efsw::FileWatcher fileWatcher( useGeneric, 100 );

		efsw::WatchID watchId = fileWatcher.addWatch( watchedDir, &listener, true );
		EXPECT_TRUE( watchId > 0 );

		fileWatcher.watch();
		sleepMs( 100 );

		std::string fileInWatched = watchedDir + "/moved_file.txt";
		EXPECT_TRUE( renameFile( fileInUnwatched, fileInWatched ) );

		listener.waitForActions( efsw::Actions::Add, "moved_file.txt" );

		EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "moved_file.txt" ) );

		listener.clearEvents();

		EXPECT_TRUE( writeFile( fileInWatched, " modified content" ) );
		sleepMs( 100 );

		listener.waitForActions( efsw::Actions::Modified, "moved_file.txt" );
		EXPECT_TRUE( listener.checkEvent( efsw::Actions::Modified, "moved_file.txt" ) );

		fileWatcher.removeWatch( watchedDir );
		removeDirectory( watchedDir );
		removeDirectory( unwatchedDir );
	}
}

UTEST( RenameWatched, FolderChildrenAddModifiedDelete ) {
	for ( auto useGeneric : fileWatchers ) {
		std::string testDir = getTemporaryDirectory();
		std::string subDir = testDir + "/watched_subdir";

		EXPECT_TRUE( createDirectory( testDir ) );
		EXPECT_TRUE( createDirectory( subDir ) );

		std::string file1 = subDir + "/child1.txt";
		std::string file2 = subDir + "/child2.txt";
		EXPECT_TRUE( createFile( file1, "content1" ) );
		EXPECT_TRUE( createFile( file2, "content2" ) );

		TestListener listener;
		efsw::FileWatcher fileWatcher( useGeneric, 100 );

		efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
		EXPECT_TRUE( watchId > 0 );

		fileWatcher.watch();
		sleepMs( 100 );

		listener.clearEvents();

		std::string renamedSubDir = testDir + "/renamed_subdir";
		EXPECT_TRUE( renameFile( subDir, renamedSubDir ) );

		listener.waitForActions( efsw::Actions::Moved, "renamed_subdir" );

		listener.clearEvents();

		std::string renamedFile1 = renamedSubDir + "/child1.txt";
		EXPECT_TRUE( writeFile( renamedFile1, " modified" ) );
		sleepMs( 200 );

		EXPECT_TRUE( listener.waitForActions( efsw::Actions::Modified, "child1.txt" ) );
		EXPECT_TRUE( listener.checkEvent( efsw::Actions::Modified, "child1.txt" ) );

		listener.clearEvents();

		std::string newFile3 = renamedSubDir + "/child3.txt";
		EXPECT_TRUE( createFile( newFile3, "content3" ) );
		sleepMs( 150 );

		listener.waitForActions( efsw::Actions::Add, "child3.txt" );
		EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "child3.txt" ) );

		listener.clearEvents();

		std::string file2Path = renamedSubDir + "/child2.txt";
		EXPECT_TRUE( removeFile( file2Path ) );
		sleepMs( 150 );

		listener.waitForActions( efsw::Actions::Delete, "child2.txt" );
		EXPECT_TRUE( listener.checkEvent( efsw::Actions::Delete, "child2.txt" ) );

		fileWatcher.removeWatch( testDir );
		removeDirectory( testDir );
	}
}
