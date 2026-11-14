#include "test_util.hpp"
#include "utest.h"

#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY

#include <cstdio>
#include <efsw/FileWatcherInotify.hpp>
#include <efsw/Lock.hpp>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <vector>

using namespace efsw_test;

namespace {

/// An inotify watcher whose action lock the test can hold, to keep the event thread waiting.
class LockableWatcher : public efsw::FileWatcherInotify {
  public:
	LockableWatcher() : FileWatcherInotify( nullptr ) {}

	efsw::Mutex& actionLock() { return mInitLock; }

	/// Waits until the event thread has read every queued event: it then only parses them before
	/// taking the action lock.
	bool waitUntilRead() {
		for ( int i = 0; i < 2000; i++ ) {
			int pending = 0;
			if ( ioctl( mFD, FIONREAD, &pending ) == 0 && pending == 0 )
				return true;
			sleepMs( 1 );
		}
		return false;
	}
};

/// Records each event with its registration and full path.
class PathListener : public efsw::FileWatchListener {
  public:
	struct Event {
		efsw::WatchID ID;
		efsw::Action Action;
		std::string Path;
	};

	std::mutex Lock;
	std::vector<Event> Events;

	void handleFileAction( efsw::WatchID id, const std::string& dir, const std::string& filename,
						   efsw::Action action, const std::string& ) override {
		std::lock_guard<std::mutex> lock( Lock );
		Events.push_back( { id, action, dir + filename } );
	}

	std::vector<Event> events() {
		std::lock_guard<std::mutex> lock( Lock );
		return Events;
	}
};

const std::vector<efsw::WatcherOption> crossMoves = {
	{ efsw::Options::ReportCrossDirectoryMoves, 1 } };

} // namespace

// A directory moved between two registrations while the destination is being removed: the event
// thread found the destination still registered, then waited for the removal to finish. The
// moved directory's watches must not be handed over to the removed registration, which would
// keep reporting to its listener after removeWatch() returned.
UTEST( InotifyMovedDirectories, NoHandoverToARemovedRegistration ) {
	const std::string root = getTemporaryDirectory() + "_handover";
	const std::string source = root + "/a";
	const std::string destination = root + "/c";
	removeDirectory( root );
	ASSERT_TRUE( createDirectory( root ) );
	ASSERT_TRUE( createDirectory( source ) );
	ASSERT_TRUE( createDirectory( source + "/d" ) );
	ASSERT_TRUE( createDirectory( source + "/d/s" ) );
	ASSERT_TRUE( createDirectory( destination ) );
	PathListener listener;
	{
		LockableWatcher watcher;
		const efsw::WatchID a = watcher.addWatch( source, &listener, true, crossMoves );
		const efsw::WatchID c = watcher.addWatch( destination, &listener, true, crossMoves );
		ASSERT_GT( a, 0 );
		ASSERT_GT( c, 0 );
		watcher.watch();
		{
			efsw::Lock lock( watcher.actionLock() );
			ASSERT_EQ( 0,
					   std::rename( ( source + "/d" ).c_str(), ( destination + "/d" ).c_str() ) );
			// The event thread reads the move, finds both registrations, and waits for the lock.
			ASSERT_TRUE( watcher.waitUntilRead() );
			sleepMs( 50 );
			watcher.removeWatch( c );
		}
		sleepMs( 200 );
		ASSERT_TRUE( createFile( destination + "/d/s/late.txt", "x" ) );
		sleepMs( 300 );
	}
	bool sourceDelete = false;
	size_t late = 0;
	for ( const PathListener::Event& event : listener.events() ) {
		// Neither registration watches the directory any more.
		if ( event.Path.find( "late.txt" ) != std::string::npos )
			late++;
		if ( event.Action == efsw::Actions::Delete && event.Path == source + "/d" )
			sourceDelete = true;
	}
	EXPECT_EQ( 0u, late );
	// The source registration lost the directory.
	EXPECT_TRUE( sourceDelete );
	removeDirectory( root );
}

#endif
