#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileSystem.hpp>

using namespace efsw_test;

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

// A relative path, even a bare name, is resolved below the working directory.
UTEST( FileSystem, CanonicalSourcePathOfARelativePath ) {
	const std::string slash( 1, efsw::FileSystem::getOSSlash() );
	const std::string base = getTemporaryDirectory() + "_relative";
	removeDirectory( base );
	EXPECT_TRUE( createDirectory( base ) );
	EXPECT_TRUE( createFile( base + slash + "file.txt", "x" ) );
	const std::string real = efsw::FileSystem::getRealPath( base );
	const std::string previous = efsw::FileSystem::getCurrentWorkingDirectory();
	ASSERT_TRUE( efsw::FileSystem::changeWorkingDirectory( base ) );
	EXPECT_TRUE( efsw::FileSystem::canonicalSourcePath( "file.txt" ) == real + slash + "file.txt" );
	EXPECT_TRUE( efsw::FileSystem::canonicalSourcePath( "gone" + slash + "file.txt" ) ==
				 real + slash + "gone" + slash + "file.txt" );
	efsw::FileSystem::changeWorkingDirectory( previous );
	removeDirectory( base );
}
