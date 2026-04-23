#include "test_util.hpp"
#include "utest.h"

using namespace efsw_test;

UTEST( Modified, SingleFile ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	std::string testFile = testDir + "/test_file.txt";
	EXPECT_TRUE( createFile( testFile, "initial content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	sleepMs( 100 );

	listener.clearEvents();

	EXPECT_TRUE( writeFile( testFile, " modified content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Modified, "test_file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Modified, MultipleWrites ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	std::string testFile = testDir + "/test_file.txt";
	EXPECT_TRUE( createFile( testFile, "line1\n" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	sleepMs( 100 );
	listener.clearEvents();

	for ( int i = 0; i < 3; i++ ) {
		EXPECT_TRUE( writeFile( testFile, "line" + std::to_string( i + 2 ) + "\n" ) );
		sleepMs( 50 );
	}

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Modified, "test_file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Modified, AppendToFile ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	std::string testFile = testDir + "/test_file.txt";
	EXPECT_TRUE( createFile( testFile, "start" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	sleepMs( 100 );
	listener.clearEvents();

	EXPECT_TRUE( writeFile( testFile, "_appended" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Modified, "test_file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}
