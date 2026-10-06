#include "test_util.hpp"
#include "utest.h"

#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <unistd.h>

using namespace efsw_test;

namespace {

struct OpenFiles {
	std::vector<int> descriptors;

	~OpenFiles() { clear(); }

	void clear() {
		for ( int fd : descriptors )
			close( fd );
		descriptors.clear();
	}
};

} // namespace

UTEST( Inotify, HighFileDescriptor ) {
	if ( useGeneric )
		UTEST_SKIP( "Requires the inotify backend" );

	struct rlimit limit;
	ASSERT_EQ( 0, getrlimit( RLIMIT_NOFILE, &limit ) );
	if ( limit.rlim_cur <= FD_SETSIZE + 16 )
		UTEST_SKIP( "Requires RLIMIT_NOFILE greater than FD_SETSIZE + 16" );

	OpenFiles files;
	do {
		int fd = open( "/dev/null", O_RDONLY );
		ASSERT_TRUE( fd >= 0 );
		files.descriptors.push_back( fd );
	} while ( files.descriptors.back() < FD_SETSIZE );

	TestListener listener;
	// The inotify descriptor must be allocated before releasing the dummy files.
	efsw::FileWatcher fileWatcher;
	files.clear();

	std::string root = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( root ) );
	efsw::WatchID watch = fileWatcher.addWatch( root, &listener, false );
	ASSERT_TRUE( watch > 0 );
	fileWatcher.watch();

	EXPECT_TRUE( createFile( root + "/created.txt", "content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "created.txt", 3000 ) );

	fileWatcher.removeWatch( watch );
	removeDirectory( root );
}

#endif
