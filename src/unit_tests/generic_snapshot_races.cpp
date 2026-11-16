#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileWatcherGeneric.hpp>
#include <efsw/WatcherGeneric.hpp>

using namespace efsw_test;

namespace {

class RemoveArrivalListener : public TestListener {
  public:
	std::string Arrival;
	std::string Renamed;

	void handleFileAction( efsw::WatchID id, const std::string& directory,
						   const std::string& filename, efsw::Action action,
						   const std::string& oldFilename ) override {
		TestListener::handleFileAction( id, directory, filename, action, oldFilename );
		if ( action == efsw::Actions::Add && filename == "trigger" ) {
			if ( Renamed.empty() )
				removeDirectory( Arrival );
			else
				renameFile( Arrival, Renamed );
		}
	}
};

} // namespace

// DirectorySnapshot already captured the arrival when the trigger's callback deletes it.
// Discovery must not discard its Add while keeping it in the snapshot for the next Delete.
UTEST( GenericSnapshotRaces, RemovedArrivalStillReportsAddBeforeDelete ) {
	const std::string root = getTemporaryDirectory() + "_generic_arrival_removed";
	ASSERT_TRUE( createDirectory( root ) );
	RemoveArrivalListener listener;
	listener.Arrival = root + "/arrival";
	{
		efsw::FileWatcher owner( true );
		efsw::FileWatcherGeneric backend( &owner, 100 );
		efsw::WatcherGeneric watch( 1, root, &listener, &backend, false );
		ASSERT_TRUE( createDirectory( listener.Arrival ) );
		ASSERT_TRUE( createFile( root + "/trigger", "x" ) );
		watch.watch();
		watch.watch();
		const auto events = listener.getEvents();
		ASSERT_EQ( 3u, events.size() );
		EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Add, "trigger" ) );
		EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Add, "arrival" ) );
		EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Delete, "arrival" ) );
	}
	removeDirectory( root );
}

UTEST( GenericSnapshotRaces, RenamedArrivalStillReportsAddBeforeMove ) {
	const std::string root = getTemporaryDirectory() + "_generic_arrival_renamed";
	ASSERT_TRUE( createDirectory( root ) );
	RemoveArrivalListener listener;
	listener.Arrival = root + "/arrival";
	listener.Renamed = root + "/renamed";
	{
		efsw::FileWatcher owner( true );
		efsw::FileWatcherGeneric backend( &owner, 100 );
		efsw::WatcherGeneric watch( 1, root, &listener, &backend, false );
		ASSERT_TRUE( createDirectory( listener.Arrival ) );
		ASSERT_TRUE( createFile( root + "/trigger", "x" ) );
		watch.watch();
		watch.watch();
		const auto events = listener.getEvents();
		ASSERT_EQ( 3u, events.size() );
		EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Add, "trigger" ) );
		EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Add, "arrival" ) );
		EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Moved, "renamed" ) );
		EXPECT_STREQ( "arrival", std::get<2>( events.back() ).c_str() );
	}
	removeDirectory( root );
}
