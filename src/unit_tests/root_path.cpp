#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>

#if defined( _WIN32 )
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace efsw_test;

namespace {

/// The root of the file system holding the temporary directory: "/", or a drive root on Windows.
std::string rootPath() {
#if defined( _WIN32 )
	const std::string temporary = getTemporaryDirectory();
	return temporary.substr( 0, 2 ) + efsw::FileSystem::getOSSlash();
#else
	return "/";
#endif
}

} // namespace

// A root's separator is the whole path: it is never removed, or the path would be empty, or name
// the current directory of a drive on Windows.
UTEST( RootPath, KeepsItsSeparator ) {
	const std::string slash( 1, efsw::FileSystem::getOSSlash() );
	EXPECT_TRUE( efsw::FileSystem::isRootPath( slash ) );
	std::string root( slash );
	efsw::FileSystem::dirRemoveSlashAtEnd( root );
	EXPECT_TRUE( root == slash );

	std::string directory( slash + "directory" + slash );
	EXPECT_FALSE( efsw::FileSystem::isRootPath( directory ) );
	efsw::FileSystem::dirRemoveSlashAtEnd( directory );
	EXPECT_TRUE( directory == slash + "directory" );
	EXPECT_FALSE( efsw::FileSystem::isRootPath( "" ) );

#if defined( _WIN32 )
	EXPECT_TRUE( efsw::FileSystem::isRootPath( "C:\\" ) );
	EXPECT_FALSE( efsw::FileSystem::isRootPath( "C:" ) );
	EXPECT_FALSE( efsw::FileSystem::isRootPath( "C:\\directory\\" ) );
	EXPECT_TRUE( efsw::FileSystem::isRootPath( "\\\\server\\share\\" ) );
	EXPECT_FALSE( efsw::FileSystem::isRootPath( "\\\\server\\share\\directory\\" ) );
	EXPECT_FALSE( efsw::FileSystem::isRootPath( "\\\\server\\" ) );
	std::string drive( "C:\\" );
	efsw::FileSystem::dirRemoveSlashAtEnd( drive );
	EXPECT_TRUE( drive == "C:\\" );
	std::string share( "\\\\server\\share\\" );
	efsw::FileSystem::dirRemoveSlashAtEnd( share );
	EXPECT_TRUE( share == "\\\\server\\share\\" );
#endif
}

// The root is a directory, whichever way its information is read.
UTEST( RootPath, InfoDescribesTheRootDirectory ) {
	const std::string root = rootPath();
	EXPECT_TRUE( efsw::FileInfo::exists( root ) );
	efsw::FileInfo info( root );
	EXPECT_TRUE( info.exists() );
	EXPECT_TRUE( info.isDirectory() );
	EXPECT_TRUE( info.Filepath == root );
	efsw::FileInfo linkInfo( root, true );
	EXPECT_TRUE( linkInfo.isDirectory() );
	EXPECT_FALSE( linkInfo.isLink() );
	EXPECT_TRUE( linkInfo.Filepath == root );
	EXPECT_TRUE( efsw::FileSystem::isDirectory( root ) );
}

// The root can be watched. When the test can write there, a file created in it is reported.
UTEST( RootPath, CanBeWatched ) {
	const std::string root = rootPath();
	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );
	const efsw::WatchID id = fileWatcher.addWatch( root, &listener, false );
	ASSERT_GT( id, 0 );
	fileWatcher.watch();
	sleepMs( 200 );

#if defined( _WIN32 )
	const std::string name = "efsw_root_test_" + std::to_string( _getpid() ) + ".txt";
#else
	const std::string name = "efsw_root_test_" + std::to_string( getpid() ) + ".txt";
#endif
	if ( createFile( root + name, "x" ) ) {
		EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, name ) );
		removeFile( root + name );
	}
	fileWatcher.removeWatch( id );
}
