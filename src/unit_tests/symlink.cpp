#include "test_util.hpp"
#include "utest.h"
#include <filesystem>

using namespace efsw_test;

UTEST( Symlink, FollowSymlinkToDirectory ) {
	std::string testDir = getTemporaryDirectory();
	std::string targetDir = testDir + "/real_target";
	std::string linkPath = testDir + "/link_to_target";

	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( targetDir ) );

	std::filesystem::create_symlink( targetDir, linkPath );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	fileWatcher.followSymlinks( true );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	sleepMs( 100 );

	listener.clearEvents();

	std::string fileInTarget = targetDir + "/file.txt";
	EXPECT_TRUE( createFile( fileInTarget, "content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file.txt" ) );

	listener.clearEvents();

	EXPECT_TRUE( writeFile( fileInTarget, "modified" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Modified, "file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Symlink, SymlinkTargetOutsideScope ) {
	std::string testDir = getTemporaryDirectory();
	std::string outsideDir = getTemporaryDirectory() + "_outside";
	std::string linkPath = testDir + "/link_to_outside";

	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( outsideDir ) );

	std::filesystem::create_symlink( outsideDir, linkPath );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	fileWatcher.followSymlinks( true );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();
	sleepMs( 100 );

	listener.clearEvents();

	std::string fileInOutside = outsideDir + "/outside_file.txt";
	EXPECT_TRUE( createFile( fileInOutside, "content" ) );
	ASSERT_FALSE( listener.waitForActions( efsw::Actions::Add, "outside_file.txt" ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
	removeDirectory( outsideDir );
}
