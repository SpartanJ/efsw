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

// Scan the intermediate registration before the file reaches the arriving tree. Its matched
// move reports the intermediate path, not the different one that the tree's first scan saves.
UTEST( GenericSnapshotRaces, FilePassesThroughAnotherWatchBeforeEnteringAnArrivingTree ) {
	const std::string root = getTemporaryDirectory() + "_generic_intermediate_arrival";
	for ( const std::string& dir : { root, root + "/first", root + "/second", root + "/third",
									 root + "/fourth", root + "/fourth/tree" } )
		ASSERT_TRUE( createDirectory( dir ) );
	ASSERT_TRUE( createFile( root + "/first/g", "x" ) );
	ASSERT_TRUE( createFile( root + "/fourth/tree/unchanged", "x" ) );
	TestListener listener;
	{
		efsw::FileWatcher owner( true );
		efsw::FileWatcherGeneric backend( &owner, 100 );
		efsw::FileActionBatch batch;
		efsw::WatcherGeneric first( 1, root + "/first", &listener, &backend, true, true );
		efsw::WatcherGeneric second( 2, root + "/second", &listener, &backend, true, true );
		efsw::WatcherGeneric third( 3, root + "/third", &listener, &backend, true, true );
		efsw::WatcherGeneric fourth( 4, root + "/fourth", &listener, &backend, true, true );
		for ( auto* watch : { &first, &second, &third, &fourth } )
			watch->SharedActionBatch = &batch;
		auto scan = [&]( efsw::WatcherGeneric& watch ) {
			watch.watch();
			batch.scanned( &watch );
		};
		ASSERT_TRUE( renameFile( root + "/fourth/tree", root + "/third/tree" ) );
		ASSERT_TRUE( renameFile( root + "/first/g", root + "/second/g" ) );
		batch.beginPass();
		scan( first );
		scan( second );
		ASSERT_TRUE( renameFile( root + "/second/g", root + "/third/tree/arrived" ) );
		scan( third );
		scan( fourth );
		batch.finishPass();
		ASSERT_EQ( 3u, listener.getEventCount() );
		EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Moved, "g" ) );
		EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Moved, "tree" ) );
		EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Add, "arrived" ) );
		for ( int pass = 0; pass < 2; pass++ ) {
			batch.beginPass();
			for ( auto* watch : { &first, &second, &third, &fourth } )
				scan( *watch );
			batch.finishPass();
		}
		EXPECT_EQ( 4u, listener.getEventCount() );
		EXPECT_EQ( 3, listener.indexOf( efsw::Actions::Delete, "g" ) );
	}
	removeDirectory( root );
}
