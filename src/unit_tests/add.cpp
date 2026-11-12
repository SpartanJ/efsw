#include "test_util.hpp"
#include "utest.h"

using namespace efsw_test;

UTEST( Add, SingleFile ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	sleepMs( 500 );

	std::string testFile = testDir + "/test_file.txt";
	EXPECT_TRUE( createFile( testFile, "test content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "test_file.txt", 3000 ) );

	fileWatcher.removeWatch( testDir );
	sleepMs( 300 );
	removeDirectory( testDir );
}

UTEST( Add, SingleFileEmoji ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	sleepMs( 500 );

	std::filesystem::path fileName = u8"test_file🐈.txt";

	std::filesystem::path testFile = testDir / fileName;
	EXPECT_TRUE( createFileFs( testFile, "test content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, fileName.u8string(), 3000 ) );

	fileWatcher.removeWatch( testDir );
	sleepMs( 300 );
	removeDirectory( testDir );
}

UTEST( Add, MultipleFiles ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	sleepMs( 100 );

	EXPECT_TRUE( createFile( testDir + "/file1.txt", "content1" ) );
	sleepMs( 300 );
	EXPECT_TRUE( createFile( testDir + "/file2.txt", "content2" ) );
	sleepMs( 300 );
	EXPECT_TRUE( createFile( testDir + "/file3.txt", "content3" ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file1.txt", 3000 ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file2.txt", 3000 ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file3.txt", 3000 ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Add, Subdirectory ) {
	std::string testDir = getTemporaryDirectory();
	std::string subDir = testDir + "/subdir";

	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( subDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	sleepMs( 100 );

	std::string testFile = subDir + "/nested_file.txt";
	EXPECT_TRUE( createFile( testFile, "nested content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "nested_file.txt", 3000 ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

// A non-recursive watch reports a subdirectory created after watching started as one of its
// entries, but does not report what happens inside it, and does not stop it from being
// registered on its own.
UTEST( Add, NonRecursiveIgnoresNewSubdirectoryContents ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );
	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, false );
	EXPECT_TRUE( watchId > 0 );
	fileWatcher.watch();
	sleepMs( 500 );

	EXPECT_TRUE( createDirectory( testDir + "/sub" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "sub", 3000 ) );
	sleepMs( 300 );
	EXPECT_TRUE( createFile( testDir + "/sub/inside.txt", "x" ) );
	sleepMs( 700 );
	EXPECT_EQ( -1, listener.indexOf( efsw::Actions::Add, "inside.txt" ) );

	TestListener subListener;
	efsw::WatchID subId = fileWatcher.addWatch( testDir + "/sub", &subListener, false );
	EXPECT_TRUE( subId > 0 );

	if ( subId > 0 )
		fileWatcher.removeWatch( subId );
	fileWatcher.removeWatch( watchId );
	sleepMs( 300 );
	removeDirectory( testDir );
}
