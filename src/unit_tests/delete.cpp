#include "test_util.hpp"
#include "utest.h"

using namespace efsw_test;

UTEST( Delete, SingleFile ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	std::string testFile = testDir + "/test_file.txt";
	EXPECT_TRUE( createFile( testFile, "test content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( waitForPending( listener, testDir ) );
	listener.clearEvents();

	EXPECT_TRUE( removeFile( testFile ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "test_file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Delete, MultipleFiles ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	EXPECT_TRUE( createFile( testDir + "/file1.txt", "content1" ) );
	EXPECT_TRUE( createFile( testDir + "/file2.txt", "content2" ) );
	EXPECT_TRUE( createFile( testDir + "/file3.txt", "content3" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( waitForPending( listener, testDir ) );
	listener.clearEvents();

	EXPECT_TRUE( removeFile( testDir + "/file1.txt" ) );
	EXPECT_TRUE( removeFile( testDir + "/file2.txt" ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "file1.txt" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "file2.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Delete, Subdirectory ) {
	std::string testDir = getTemporaryDirectory();
	std::string subDir = testDir + "/subdir";

	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( subDir ) );

	std::string testFile = subDir + "/nested_file.txt";
	EXPECT_TRUE( createFile( testFile, "nested content" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	EXPECT_TRUE( waitForPending( listener, testDir ) );
	listener.clearEvents();

	EXPECT_TRUE( removeFile( testFile ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "nested_file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

// Files deleted one after another, the first by name first: each Delete is reported. kqueue
// keeps a sorted list of its files' descriptors, which a removal left unsorted, so that a later
// one could no longer be found, and its Delete was never reported.
UTEST( Delete, FilesInTurn ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createFile( testDir + "/a.txt", "a" ) );
	EXPECT_TRUE( createFile( testDir + "/b.txt", "b" ) );
	EXPECT_TRUE( createFile( testDir + "/c.txt", "c" ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	EXPECT_TRUE( waitForPending( listener, testDir ) );
	listener.clearEvents();

	EXPECT_TRUE( removeFile( testDir + "/a.txt" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "a.txt" ) );
	EXPECT_TRUE( removeFile( testDir + "/c.txt" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "c.txt" ) );
	EXPECT_TRUE( removeFile( testDir + "/b.txt" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "b.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}
