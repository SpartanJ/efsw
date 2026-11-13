#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>

using namespace efsw_test;

UTEST( FileInfo, Exists ) {
	const std::string directory = getTemporaryDirectory();
	const std::string file = directory + "/present.txt";
	const std::string missing = directory + "/missing.txt";
	const std::string slash( 1, efsw::FileSystem::getOSSlash() );
	EXPECT_TRUE( createDirectory( directory ) );
	EXPECT_TRUE( createFile( file, "content" ) );

	EXPECT_TRUE( efsw::FileInfo::exists( file ) );
	EXPECT_TRUE( efsw::FileInfo::exists( directory + slash ) );
	// Historical behavior: a trailing slash is stripped before checking, even for files.
	EXPECT_TRUE( efsw::FileInfo::exists( file + slash ) );
	EXPECT_FALSE( efsw::FileInfo::exists( missing ) );

	efsw::FileInfo info( file );
	EXPECT_TRUE( info.exists() );
	EXPECT_TRUE( info.Filepath == file );
	efsw::FileInfo withSlash( file + slash );
	EXPECT_TRUE( withSlash.exists() );
	EXPECT_TRUE( withSlash.Filepath == file + slash );
	removeDirectory( directory );
}

#if !defined( _WIN32 )
// A path that does not exist cannot be resolved: no partial result. Windows resolves paths
// lexically, which also works for missing ones.
UTEST( FileSystem, RealPathOfAMissingPathIsEmpty ) {
	const std::string missing = getTemporaryDirectory() + "_missing/below/file";
	removeDirectory( getTemporaryDirectory() + "_missing" );
	EXPECT_TRUE( efsw::FileSystem::getRealPath( missing ).empty() );
}
#endif

// The source of a move may be gone with its directory: the deepest directory that still exists
// is resolved, and the rest of the path kept.
UTEST( FileSystem, CanonicalSourcePathKeepsMissingDirectories ) {
	const std::string slash( 1, efsw::FileSystem::getOSSlash() );
	const std::string base = getTemporaryDirectory() + "_canonical";
	removeDirectory( base );
	EXPECT_TRUE( createDirectory( base ) );
	const std::string real = efsw::FileSystem::getRealPath( base );
	EXPECT_FALSE( real.empty() );
	EXPECT_TRUE( efsw::FileSystem::canonicalSourcePath( base + slash + "file" ) ==
				 real + slash + "file" );
	EXPECT_TRUE( efsw::FileSystem::canonicalSourcePath( base + slash + "gone" + slash + "deeper" +
														slash + "file" ) ==
				 real + slash + "gone" + slash + "deeper" + slash + "file" );
	removeDirectory( base );
}
