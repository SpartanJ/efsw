#include "test_util.hpp"
#include "utest.h"

#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <efsw/FileWatcherInotify.hpp>
#include <efsw/Lock.hpp>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

using namespace efsw_test;

namespace {

/// Polls a condition until it holds or two seconds pass.
bool pollUntil( const std::function<bool()>& condition ) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2 );
	while ( !condition() ) {
		if ( std::chrono::steady_clock::now() > deadline )
			return false;
		std::this_thread::yield();
	}
	return true;
}

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

	/// Waits until the event thread has read every queued event, then until it waits for the
	/// action lock, which the caller holds.
	bool waitUntilBlocked() {
		if ( !pollUntil( [this] {
				 int pending = 0;
				 return ioctl( mFD, FIONREAD, &pending ) == 0 && pending == 0;
			 } ) )
			return false;
		return pollUntil( [] { return anotherThreadWaitsOnALock(); } );
	}

  private:
	/// Whether a thread of the process, other than the caller, sleeps on a futex: the event
	/// thread, the only other one, waiting for the lock. Where the kernel does not tell, assumed.
	static bool anotherThreadWaitsOnALock() {
		const long self = syscall( SYS_gettid );
		DIR* tasks = opendir( "/proc/self/task" );
		if ( !tasks )
			return true;
		bool waits = false;
		bool known = false;
		while ( dirent* task = readdir( tasks ) ) {
			if ( task->d_name[0] == '.' || atol( task->d_name ) == self )
				continue;
			std::ifstream file( std::string( "/proc/self/task/" ) + task->d_name + "/wchan" );
			std::string channel;
			std::getline( file, channel );
			if ( !channel.empty() && channel != "0" )
				known = true;
			if ( channel.find( "futex" ) != std::string::npos )
				waits = true;
		}
		closedir( tasks );
		return waits || !known;
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

	void handleFileAction( efsw::WatchID id, const std::string& dir, const std::string& filename,
						   efsw::Action action, const std::string& ) override {
		std::lock_guard<std::mutex> lock( mLock );
		mEvents.push_back( { id, action, dir + filename } );
		mChanged.notify_all();
	}

	std::vector<Event> events() {
		std::lock_guard<std::mutex> lock( mLock );
		return mEvents;
	}

	/// Waits for an event of the action on the path.
	bool waitFor( efsw::Action action, const std::string& path ) {
		std::unique_lock<std::mutex> lock( mLock );
		return mChanged.wait_for( lock, std::chrono::seconds( 2 ), [&] {
			for ( const Event& event : mEvents )
				if ( event.Action == action && event.Path == path )
					return true;
			return false;
		} );
	}

	/// Creates a file in a watched directory and waits for its Add: the events queued before it
	/// were handled, since inotify reports them in order.
	bool settle( const std::string& directory ) {
		const std::string marker = directory + "/marker" + std::to_string( mMarkers++ );
		return createFile( marker, "" ) && waitFor( efsw::Actions::Add, marker );
	}

  private:
	std::mutex mLock;
	std::condition_variable mChanged;
	std::vector<Event> mEvents;
	int mMarkers{ 0 };
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
		ASSERT_GT( watcher.addWatch( source, &listener, true, crossMoves ), 0 );
		const efsw::WatchID c = watcher.addWatch( destination, &listener, true, crossMoves );
		ASSERT_GT( c, 0 );
		watcher.watch();
		{
			efsw::Lock lock( watcher.actionLock() );
			ASSERT_EQ( 0,
					   std::rename( ( source + "/d" ).c_str(), ( destination + "/d" ).c_str() ) );
			// The event thread reads the move, finds both registrations, and waits for the lock.
			ASSERT_TRUE( watcher.waitUntilBlocked() );
			watcher.removeWatch( c );
		}
		// The source registration lost the directory.
		EXPECT_TRUE( listener.waitFor( efsw::Actions::Delete, source + "/d" ) );
		ASSERT_TRUE( createFile( destination + "/d/s/late.txt", "x" ) );
		ASSERT_TRUE( listener.settle( source ) );
	}
	// Neither registration watches the directory any more.
	for ( const PathListener::Event& event : listener.events() )
		EXPECT_EQ( std::string::npos, event.Path.find( "late.txt" ) );
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
		// Reported once its watch is installed.
		ASSERT_TRUE( createDirectory( first + "/d" ) );
		ASSERT_TRUE( listener.waitFor( efsw::Actions::Add, first + "/d" ) );
		{
			// The event thread waits for the lock to report an Add, while both moves are queued
			// behind it, to be read together. One event, so it has nothing left to read.
			efsw::Lock lock( watcher.actionLock() );
			ASSERT_TRUE( createDirectory( first + "/wake" ) );
			ASSERT_TRUE( watcher.waitUntilBlocked() );
			ASSERT_EQ( 0, std::rename( ( first + "/d" ).c_str(), ( first + "/e" ).c_str() ) );
			ASSERT_EQ( 0, std::rename( ( first + "/e" ).c_str(), ( second + "/e" ).c_str() ) );
		}
		ASSERT_TRUE( listener.settle( second ) );
		ASSERT_TRUE( createFile( second + "/e/x.txt", "x" ) );
		EXPECT_TRUE( listener.waitFor( efsw::Actions::Add, second + "/e/x.txt" ) );
		ASSERT_TRUE( listener.settle( first ) );
	}
	// The first registration reports nothing inside the directory that left it.
	for ( const PathListener::Event& event : listener.events() )
		if ( event.ID == a )
			EXPECT_EQ( std::string::npos, event.Path.find( "x.txt" ) );
	removeDirectory( root );
}

// A directory created, then its parent moved, before the creation was handled: the watch of the
// new directory is installed where the move put it, within the registration or in the one the
// parent moved to, so what happens inside it is reported.
UTEST( InotifyMovedDirectories, NewDirectoryWhoseParentMovedOn ) {
	for ( bool toOtherRegistration : { false, true } ) {
		const std::string root = getTemporaryDirectory() + "_parent_moved";
		const std::string first = root + "/a";
		const std::string second = root + "/b";
		removeDirectory( root );
		ASSERT_TRUE( createDirectory( root ) );
		ASSERT_TRUE( createDirectory( first ) );
		ASSERT_TRUE( createDirectory( second ) );
		ASSERT_TRUE( createDirectory( first + "/p" ) );
		const std::string moved = toOtherRegistration ? second + "/q" : first + "/q";
		PathListener listener;
		{
			LockableWatcher watcher;
			ASSERT_GT( watcher.addWatch( first, &listener, true, crossMoves ), 0 );
			ASSERT_GT( watcher.addWatch( second, &listener, true, crossMoves ), 0 );
			watcher.watch();
			{
				// The event thread waits for the lock to report the wake-up, while the
				// creation and the move are queued behind it, to be read together.
				efsw::Lock lock( watcher.actionLock() );
				ASSERT_TRUE( createDirectory( first + "/wake" ) );
				ASSERT_TRUE( watcher.waitUntilBlocked() );
				ASSERT_TRUE( createDirectory( first + "/p/d" ) );
				ASSERT_EQ( 0, std::rename( ( first + "/p" ).c_str(), moved.c_str() ) );
			}
			ASSERT_TRUE( listener.settle( first ) );
			ASSERT_TRUE( createFile( moved + "/d/x.txt", "x" ) );
			EXPECT_TRUE( listener.waitFor( efsw::Actions::Add, moved + "/d/x.txt" ) );
		}
		removeDirectory( root );
	}
}

#endif
