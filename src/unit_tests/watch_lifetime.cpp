#include "test_util.hpp"
#include "utest.h"
#include "watch_lifetime_util.hpp"
#include <memory>

using namespace efsw_test;

namespace {
void deletedDirectoryShutdown( int* utest_result, int removal ) {
	ShutdownDeadline deadline;
	for ( bool recursive : { false, true } ) {
		for ( int delay : { 0, 200 } ) {
			std::string dir =
				std::filesystem::canonical( std::filesystem::temp_directory_path() ).string() +
				"/efsw_lifetime_" +
				std::filesystem::path( getTemporaryDirectory() ).filename().string();
			ASSERT_TRUE( createDirectory( dir ) );
			ASSERT_TRUE( createDirectory( dir + "/child" ) );
			ASSERT_TRUE( createFile( dir + "/child/file.txt", "content" ) );
			TestListener listener;
			{
				efsw::FileWatcher watcher( useGeneric, 20 );
				auto id = watcher.addWatch( dir, &listener, recursive,
											{ { efsw::Options::ReportCrossDirectoryMoves, 1 } } );
				ASSERT_GT( id, 0 );
				watcher.watch();
				sleepMs( 50 );
				std::error_code error;
				ASSERT_GT( std::filesystem::remove_all( dir, error ), 0u );
				ASSERT_FALSE( bool( error ) );
				sleepMs( delay );
				if ( removal == 1 )
					watcher.removeWatch( id );
				else if ( removal == 2 )
					watcher.removeWatch( dir );
				if ( removal != 0 )
					EXPECT_TRUE( watcher.directories().empty() );
			}
		}
	}
}

class BlockingListener : public efsw::FileWatchListener {
  public:
	std::mutex mutex;
	std::condition_variable cv;
	bool entered = false;
	bool released = false;
	std::string observedDirectory;

	void handleFileAction( efsw::WatchID, const std::string& dir, const std::string&, efsw::Action,
						   const std::string& ) override {
		std::unique_lock<std::mutex> lock( mutex );
		entered = true;
		cv.notify_one();
		cv.wait( lock, [this] { return released; } );
		// Keep using callback arguments after shutdown starts to catch premature watch deletion.
		observedDirectory = dir;
	}
};
} // namespace

UTEST( WatchLifetime, DeletedDirectoryThenDestroy ) {
	deletedDirectoryShutdown( utest_result, 0 );
}

UTEST( WatchLifetime, DeletedDirectoryThenRemoveById ) {
	deletedDirectoryShutdown( utest_result, 1 );
}

UTEST( WatchLifetime, DeletedDirectoryThenRemoveByPath ) {
	deletedDirectoryShutdown( utest_result, 2 );
}

UTEST( WatchLifetime, DestroyDuringCallback ) {
	ShutdownDeadline deadline;
	std::string dir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( dir ) );
	BlockingListener listener;
	std::unique_ptr<efsw::FileWatcher> watcher( new efsw::FileWatcher( useGeneric, 20 ) );
	ASSERT_GT( watcher->addWatch( dir, &listener, false ), 0 );
	watcher->watch();
	ASSERT_TRUE( createFile( dir + "/callback.txt" ) );
	bool entered;
	{
		std::unique_lock<std::mutex> lock( listener.mutex );
		entered = listener.cv.wait_for( lock, std::chrono::seconds( 3 ),
										[&] { return listener.entered; } );
	}
	std::thread releaseCallback( [&] {
		sleepMs( 100 );
		std::lock_guard<std::mutex> lock( listener.mutex );
		listener.released = true;
		listener.cv.notify_one();
	} );
	watcher.reset();
	releaseCallback.join();
	EXPECT_TRUE( entered );
	EXPECT_FALSE( listener.observedDirectory.empty() );
	removeDirectory( dir );
}
