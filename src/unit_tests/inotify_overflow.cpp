#include "test_util.hpp"
#include "utest.h"

#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY

#include <efsw/FileSystem.hpp>
#include <efsw/FileWatcherInotify.hpp>
#include <map>
#include <mutex>

using namespace efsw_test;

namespace {

class MissedListener : public TestListener {
  public:
	std::mutex missedLock;
	std::map<std::string, int> missed;

	void handleMissedFileActions( efsw::WatchID, const std::string& directory ) override {
		std::lock_guard<std::mutex> lock( missedLock );
		missed[directory]++;
	}
};

class OverflowingWatcher : public efsw::FileWatcherInotify {
  public:
	OverflowingWatcher() : FileWatcherInotify( nullptr ) {}

	void overflow() { reportOverflow(); }
};

std::string withSlash( std::string directory ) {
	efsw::FileSystem::dirAddSlashAtEnd( directory );
	return directory;
}

} // namespace

// The event queue overflowed: every registration is told once, a recursive one with several
// native watches included, and a removed one is not.
UTEST( InotifyOverflow, EveryRegistrationIsToldOnce ) {
	const std::string recursive = getTemporaryDirectory() + "_overflow_recursive";
	const std::string plain = getTemporaryDirectory() + "_overflow_plain";
	const std::string removed = getTemporaryDirectory() + "_overflow_removed";
	for ( const std::string& dir : { recursive, plain, removed } ) {
		removeDirectory( dir );
		ASSERT_TRUE( createDirectory( dir ) );
	}
	ASSERT_TRUE( createDirectory( recursive + "/a" ) );
	ASSERT_TRUE( createDirectory( recursive + "/a/b" ) );
	MissedListener recursiveListener, plainListener, removedListener;
	{
		OverflowingWatcher watcher;
		ASSERT_GT( watcher.addWatch( recursive, &recursiveListener, true, {} ), 0 );
		ASSERT_GT( watcher.addWatch( plain, &plainListener, false, {} ), 0 );
		const efsw::WatchID gone = watcher.addWatch( removed, &removedListener, false, {} );
		ASSERT_GT( gone, 0 );
		watcher.removeWatch( gone );

		watcher.overflow();
	}
	ASSERT_EQ( 1u, recursiveListener.missed.size() );
	EXPECT_EQ( 1, recursiveListener.missed[withSlash( recursive )] );
	ASSERT_EQ( 1u, plainListener.missed.size() );
	EXPECT_EQ( 1, plainListener.missed[withSlash( plain )] );
	EXPECT_TRUE( removedListener.missed.empty() );
	for ( const std::string& dir : { recursive, plain, removed } )
		removeDirectory( dir );
}

#endif
