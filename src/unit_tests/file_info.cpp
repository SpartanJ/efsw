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
