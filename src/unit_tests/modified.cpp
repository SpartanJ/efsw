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

	EXPECT_TRUE( waitForPending( listener, testDir ) );

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

	EXPECT_TRUE( waitForPending( listener, testDir ) );
	listener.clearEvents();

	for ( int i = 0; i < 3; i++ ) {
		EXPECT_TRUE( writeFile( testFile, "line" + std::to_string( i + 2 ) + "\n" ) );
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

	EXPECT_TRUE( waitForPending( listener, testDir ) );
	listener.clearEvents();

	EXPECT_TRUE( writeFile( testFile, "_appended" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Modified, "test_file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

// A file replaced at its path by another one, then written: the write is reported, though
// kqueue watches each file through a descriptor opened on the replaced one.
UTEST( Modified, FileReplacedAtItsPath ) {
	std::string root = getTemporaryDirectory();
	std::string testDir = root + "/watched";
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createFile( testDir + "/test_file.txt", "old" ) );
	EXPECT_TRUE( createFile( root + "/replacement.txt", "new" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	EXPECT_TRUE( waitForPending( listener, testDir ) );
	EXPECT_TRUE( renameFile( root + "/replacement.txt", testDir + "/test_file.txt" ) );
	EXPECT_TRUE( waitForPending( listener, testDir ) );
	listener.clearEvents();

	EXPECT_TRUE( writeFile( testDir + "/test_file.txt", " written" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Modified, "test_file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( root );
}
