#include "test_util.hpp"
#include "utest.h"
#include <filesystem>

using namespace efsw_test;

static bool symlinkUnavailable( const std::filesystem::filesystem_error& error ) {
#if defined( _WIN32 )
	const int code = error.code().value();
	// Win32 ERROR_ACCESS_DENIED, ERROR_CALL_NOT_IMPLEMENTED, ERROR_NOT_SUPPORTED,
	// and ERROR_PRIVILEGE_NOT_HELD respectively.
	return code == 5 || code == 38 || code == 50 || code == 120 || code == 1314 ||
		   error.code() == std::errc::function_not_supported ||
		   error.code() == std::errc::operation_not_supported ||
		   error.code() == std::errc::permission_denied;
#else
	return error.code() == std::errc::permission_denied ||
		   error.code() == std::errc::operation_not_supported;
#endif
}

UTEST( Symlink, FollowSymlinkToDirectory ) {
	std::string testDir = getTemporaryDirectory();
	std::string targetDir = testDir + "/real_target";
	std::string linkPath = testDir + "/link_to_target";

	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( targetDir ) );

	try {
		std::filesystem::create_symlink( targetDir, linkPath );
	} catch ( const std::filesystem::filesystem_error& error ) {
		if ( !symlinkUnavailable( error ) )
			throw;
		removeDirectory( testDir );
		UTEST_SKIP( "symbolic links are unavailable in this environment" );
	}

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

	try {
		std::filesystem::create_symlink( outsideDir, linkPath );
	} catch ( const std::filesystem::filesystem_error& error ) {
		if ( !symlinkUnavailable( error ) )
			throw;
		removeDirectory( testDir );
		removeDirectory( outsideDir );
		UTEST_SKIP( "symbolic links are unavailable in this environment" );
	}

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
