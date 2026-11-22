#include "utest.h"
#include <efsw/FileActionBatch.hpp>

namespace {

struct RecordedEvent {
	efsw::WatchID ID;
	efsw::Action Action;
	std::string Directory;
	std::string Filename;
	std::string OldFilename;
};

struct RecordingListener : efsw::FileWatchListener {
	std::vector<RecordedEvent> Events;
	efsw::FileActionBatch* CancelDuringCallback{ nullptr };

	void handleFileAction( efsw::WatchID id, const std::string& directory,
						   const std::string& filename, efsw::Action action,
						   const std::string& oldFilename ) override {
		if ( CancelDuringCallback )
			CancelDuringCallback->cancel();
		// Read the callback's references after cancellation: retiring the watch must not
		// free them while the callback is still running.
		Events.push_back( { id, action, directory, filename, oldFilename } );
	}
};

struct InspectableBatch : efsw::FileActionBatch {
	size_t capacity() const { return mEvents.capacity(); }
};

} // namespace

UTEST( FileActionBatch, PreservesCollectedOrderAndExistingMoves ) {
	efsw::FileActionBatch batch;
	RecordingListener listener;
	batch.add( 7, "root/", "new", efsw::Actions::Add, "" );
	batch.add( 7, "root/", "new", efsw::Actions::Modified, "" );
	batch.add( 7, "root/", "renamed", efsw::Actions::Moved, "new" );
	batch.add( 7, "root/", "renamed", efsw::Actions::Delete, "" );
	EXPECT_TRUE( listener.Events.empty() );
	batch.dispatch( &listener );
	ASSERT_EQ( 4u, listener.Events.size() );
	const efsw::Action actions[] = { efsw::Actions::Add, efsw::Actions::Modified,
									 efsw::Actions::Moved, efsw::Actions::Delete };
	for ( size_t i = 0; i < listener.Events.size(); ++i ) {
		EXPECT_EQ( actions[i], listener.Events[i].Action );
		EXPECT_EQ( 7, listener.Events[i].ID );
		EXPECT_STREQ( "root/", listener.Events[i].Directory.c_str() );
		EXPECT_STREQ( i < 2 ? "new" : "renamed", listener.Events[i].Filename.c_str() );
		EXPECT_STREQ( i == 2 ? "new" : "", listener.Events[i].OldFilename.c_str() );
	}
	batch.dispatch( &listener );
	EXPECT_EQ( 4u, listener.Events.size() );
}

UTEST( FileActionBatch, ClearAndDispatchResetCancellation ) {
	efsw::FileActionBatch batch;
	RecordingListener listener;
	batch.add( 1, "root/", "dropped", efsw::Actions::Add, "" );
	batch.cancel();
	batch.dispatch( &listener );
	EXPECT_TRUE( listener.Events.empty() );
	batch.add( 1, "root/", "cleared", efsw::Actions::Add, "" );
	batch.cancel();
	batch.clear();
	batch.add( 2, "root/", "kept", efsw::Actions::Add, "" );
	batch.dispatch( &listener );
	ASSERT_EQ( 1u, listener.Events.size() );
	EXPECT_EQ( 2, listener.Events[0].ID );
	EXPECT_STREQ( "kept", listener.Events[0].Filename.c_str() );
}

UTEST( FileActionBatch, CancellationKeepsTheRunningCallbacksReferencesAlive ) {
	efsw::FileActionBatch batch;
	RecordingListener listener;
	listener.CancelDuringCallback = &batch;
	const std::string directory( 200, 'd' );
	const std::string filename( 200, 'f' );
	const std::string oldFilename( 200, 'o' );
	batch.add( 3, directory, filename, efsw::Actions::Moved, oldFilename );
	batch.add( 3, directory, "must-not-arrive", efsw::Actions::Add, "" );
	batch.dispatch( &listener );
	ASSERT_EQ( 1u, listener.Events.size() );
	EXPECT_STREQ( directory.c_str(), listener.Events[0].Directory.c_str() );
	EXPECT_STREQ( filename.c_str(), listener.Events[0].Filename.c_str() );
	EXPECT_STREQ( oldFilename.c_str(), listener.Events[0].OldFilename.c_str() );
	listener.CancelDuringCallback = nullptr;
	batch.add( 3, directory, "next-scan", efsw::Actions::Add, "" );
	batch.dispatch( &listener );
	ASSERT_EQ( 2u, listener.Events.size() );
	EXPECT_STREQ( "next-scan", listener.Events[1].Filename.c_str() );
}

UTEST( FileActionBatch, NullListenerDropsEvents ) {
	efsw::FileActionBatch batch;
	RecordingListener listener;
	batch.add( 1, "root/", "dropped", efsw::Actions::Add, "" );
	batch.dispatch( nullptr );
	batch.dispatch( &listener );
	EXPECT_TRUE( listener.Events.empty() );
}

UTEST( FileActionBatch, ReusesSteadyCapacityAndReleasesABurstAfterASmallScan ) {
	InspectableBatch batch;
	RecordingListener listener;
	for ( int pass = 0; pass < 2; ++pass ) {
		batch.clear();
		for ( size_t i = 0; i < 2048; ++i )
			batch.add( 1, "root/", "file", efsw::Actions::Add, "" );
		const size_t capacity = batch.capacity();
		ASSERT_GE( capacity, 2048u );
		batch.dispatch( &listener );
		EXPECT_EQ( capacity, batch.capacity() );
	}
	EXPECT_EQ( 4096u, listener.Events.size() );
	batch.add( 1, "root/", "small-scan", efsw::Actions::Add, "" );
	batch.dispatch( &listener );
	EXPECT_EQ( 0u, batch.capacity() );
	EXPECT_EQ( 4097u, listener.Events.size() );
}
