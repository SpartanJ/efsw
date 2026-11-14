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

	// The event thread makes virtual calls: it stops before this class is destroyed.
	~LockableWatcher() override {
		mInitOK = false;
		efSAFE_DELETE( mThread );
	}

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

// A directory renamed, then moved to another registration before the rename was handled: its
// watch must follow it to where the rename put it, so the move takes it away from the first
// registration, which no longer reports what happens inside it.
UTEST( InotifyMovedDirectories, RenamedThenMovedAway ) {
	const std::string root = getTemporaryDirectory() + "_renamed_moved";
	const std::string first = root + "/a";
	const std::string second = root + "/b";
	removeDirectory( root );
	ASSERT_TRUE( createDirectory( root ) );
	ASSERT_TRUE( createDirectory( first ) );
	ASSERT_TRUE( createDirectory( second ) );
	PathListener listener;
	efsw::WatchID a = 0;
	{
		LockableWatcher watcher;
		a = watcher.addWatch( first, &listener, true, {} );
		ASSERT_GT( a, 0 );
		ASSERT_GT( watcher.addWatch( second, &listener, true, {} ), 0 );
		watcher.watch();
		ASSERT_TRUE( createDirectory( first + "/d" ) );
		sleepMs( 200 );
		{
			// The event thread waits for the lock to report an Add, while both moves are queued
			// behind it, to be read together.
			efsw::Lock lock( watcher.actionLock() );
			// One event, so the thread blocks with nothing left to read.
			ASSERT_TRUE( createDirectory( first + "/wake" ) );
			ASSERT_TRUE( watcher.waitUntilRead() );
			sleepMs( 50 );
			ASSERT_EQ( 0, std::rename( ( first + "/d" ).c_str(), ( first + "/e" ).c_str() ) );
			ASSERT_EQ( 0, std::rename( ( first + "/e" ).c_str(), ( second + "/e" ).c_str() ) );
		}
		sleepMs( 200 );
		ASSERT_TRUE( createFile( second + "/e/x.txt", "x" ) );
		sleepMs( 300 );
	}
	size_t inFirst = 0;
	bool inSecond = false;
	for ( const PathListener::Event& event : listener.events() ) {
		if ( event.Path.find( "x.txt" ) == std::string::npos )
			continue;
		if ( event.ID == a )
			inFirst++;
		else if ( event.Path == second + "/e/x.txt" )
			inSecond = true;
	}
	// The first registration reports nothing inside the directory that left it.
	EXPECT_EQ( 0u, inFirst );
	EXPECT_TRUE( inSecond );
	removeDirectory( root );
}

#endif
