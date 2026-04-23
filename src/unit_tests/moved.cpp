#include "test_util.hpp"
#include "utest.h"

using namespace efsw_test;

UTEST( Moved, RenameFile ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	std::string oldFile = testDir + "/old_name.txt";
	std::string newFile = testDir + "/new_name.txt";
	EXPECT_TRUE( createFile( oldFile, "test content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	sleepMs( 200 );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( oldFile, newFile ) );

	listener.waitForActions( efsw::Actions::Moved, "new_name.txt" );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Moved, "new_name.txt", "old_name.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Moved, MoveFileToSubdirectory ) {
	std::string testDir = getTemporaryDirectory();
	std::string subDir = testDir + "/subdir";

	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( subDir ) );

	std::string sourceFile = testDir + "/file.txt";
	std::string destFile = subDir + "/file.txt";
	EXPECT_TRUE( createFile( sourceFile, "test content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	sleepMs( 200 );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( sourceFile, destFile ) );

	listener.waitForActions( efsw::Actions::Add, "file.txt" );
	listener.waitForActions( efsw::Actions::Delete, "file.txt" );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Moved, MoveFileBetweenDirectories ) {
	std::string testDir = getTemporaryDirectory();
	std::string dir1 = testDir + "/dir1";
	std::string dir2 = testDir + "/dir2";

	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( dir1 ) );
	EXPECT_TRUE( createDirectory( dir2 ) );

	std::string sourceFile = dir1 + "/file.txt";
	std::string destFile = dir2 + "/file.txt";
	EXPECT_TRUE( createFile( sourceFile, "test content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	sleepMs( 200 );
	listener.clearEvents();

	EXPECT_TRUE( renameFile( sourceFile, destFile ) );

	listener.waitForActions( efsw::Actions::Add, "file.txt" );
	listener.waitForActions( efsw::Actions::Delete, "file.txt" );

	EXPECT_TRUE( listener.checkEvent( efsw::Actions::Add, "file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}
