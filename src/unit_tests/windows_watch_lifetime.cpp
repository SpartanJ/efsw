#include "test_util.hpp"
#include "watch_lifetime_util.hpp"
#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32
#include <atomic>
#include <efsw/FileWatcherWin32.hpp>
#include <efsw/Lock.hpp>

// utest must see the Windows headers before declaring its timer functions.
#include "utest.h"

using namespace efsw_test;

namespace {
class MissedListener : public TestListener {
  public:
	std::atomic<bool> missed{ false };
	void handleMissedFileActions( efsw::WatchID, const std::string& ) override { missed = true; }
};

class FailingWatch : public efsw::FileWatcherWin32 {
  public:
	FailingWatch() : FileWatcherWin32( nullptr ) {}

	void closeDirectory() {
		efsw::Lock lock( mWatchesLock );
		auto watch = *mWatches.begin();
		CancelIoEx( watch->Watch->DirHandle, &watch->Overlapped );
		CloseHandle( watch->Watch->DirHandle );
		watch->Watch->DirHandle = INVALID_HANDLE_VALUE;
	}

	bool waitForFailedRearm( const MissedListener& listener ) {
		for ( int i = 0; i < 300; ++i ) {
			{
				efsw::Lock lock( mWatchesLock );
				// The dispatch lock ensures the callback and re-arm attempt have returned.
				if ( listener.missed )
					return true;
			}
			sleepMs( 10 );
		}
		return false;
	}
};

class RemoveOnMissedListener : public TestListener {
  public:
	efsw::FileWatcherWin32* watcher = nullptr;
	bool removed = false;

	void handleMissedFileActions( efsw::WatchID id, const std::string& ) override {
		watcher->removeWatch( id );
		std::lock_guard<std::mutex> lock( mtx );
		removed = true;
		cv.notify_one();
	}

	bool waitForRemoval() {
		std::unique_lock<std::mutex> lock( mtx );
		return cv.wait_for( lock, std::chrono::seconds( 3 ), [this] { return removed; } );
	}
};
} // namespace

UTEST( WindowsWatchLifetime, FailedRearmThenRemove ) {
	ShutdownDeadline deadline;
	std::string dir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( dir ) );
	MissedListener listener;
	{
		FailingWatch watcher;
		auto id = watcher.addWatch( dir, &listener, false, {} );
		ASSERT_GT( id, 0 );
		// Force the same re-arm failure as a deleted directory, even under Wine.
		watcher.closeDirectory();
		watcher.watch();
		ASSERT_TRUE( watcher.waitForFailedRearm( listener ) );
		watcher.removeWatch( id );
	}
	removeDirectory( dir );
}

UTEST( WindowsWatchLifetime, FailedRearmThenDestroy ) {
	ShutdownDeadline deadline;
	std::string dir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( dir ) );
	MissedListener listener;
	{
		FailingWatch watcher;
		ASSERT_GT( watcher.addWatch( dir, &listener, false, {} ), 0 );
		watcher.closeDirectory();
		watcher.watch();
		ASSERT_TRUE( watcher.waitForFailedRearm( listener ) );
	}
	removeDirectory( dir );
}

UTEST( WindowsWatchLifetime, DeletedDirectoryThenRemove ) {
	ShutdownDeadline deadline;
	std::string dir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( dir ) );
	TestListener listener;
	{
		efsw::FileWatcher watcher;
		auto id = watcher.addWatch( dir, &listener, false );
		ASSERT_GT( id, 0 );
		watcher.watch();
		sleepMs( 200 );
		ASSERT_TRUE( std::filesystem::remove( dir ) );
		sleepMs( 500 );
		watcher.removeWatch( id );
	}
}

UTEST( WindowsWatchLifetime, DeletedDirectoryThenDestroy ) {
	ShutdownDeadline deadline;
	std::string dir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( dir ) );
	TestListener listener;
	{
		efsw::FileWatcher watcher;
		ASSERT_GT( watcher.addWatch( dir, &listener, false ), 0 );
		watcher.watch();
		sleepMs( 200 );
		ASSERT_TRUE( std::filesystem::remove( dir ) );
		sleepMs( 500 );
	}
}

UTEST( WindowsWatchLifetime, DestroyBeforeWatching ) {
	ShutdownDeadline deadline;
	std::string dir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( dir ) );
	TestListener listener;
	{
		efsw::FileWatcher watcher;
		ASSERT_GT( watcher.addWatch( dir, &listener, false ), 0 );
	}
	removeDirectory( dir );
}

UTEST( WindowsWatchLifetime, RemoveFromMissedEventsCallback ) {
	ShutdownDeadline deadline;
	std::string dir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( dir ) );
	RemoveOnMissedListener listener;
	{
		FailingWatch watcher;
		listener.watcher = &watcher;
		ASSERT_GT( watcher.addWatch( dir, &listener, false, {} ), 0 );
		watcher.closeDirectory();
		watcher.watch();
		ASSERT_TRUE( listener.waitForRemoval() );
	}
	removeDirectory( dir );
}

UTEST( WindowsWatchLifetime, DestroyWithQueuedCompletion ) {
	ShutdownDeadline deadline;
	std::string dir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( dir ) );
	TestListener listener;
	{
		efsw::FileWatcher watcher;
		ASSERT_GT( watcher.addWatch( dir, &listener, false ), 0 );
		ASSERT_TRUE( createFile( dir + "/queued.txt" ) );
		sleepMs( 200 );
		// No worker was started: even completed I/O still owns an undequeued packet.
	}
	removeDirectory( dir );
}
#endif
