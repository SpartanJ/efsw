#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileSystem.hpp>
#include <efsw/agent_protocol.hpp>
#include <efsw/base.hpp>
#include <atomic>
#include <thread>
#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
#include <fcntl.h>
#endif

using namespace efsw_test;

namespace {

const std::vector<efsw::WatcherOption> crossMoveOptions = {
	{ efsw::Options::ReportCrossDirectoryMoves, 1 } };

struct TwoWatches {
	std::string Source;
	std::string Destination;
	TestListener SourceListener;
	TestListener DestinationListener;
	efsw::FileWatcher Watcher;
	efsw::WatchID SourceId{ 0 };
	efsw::WatchID DestinationId{ 0 };

	TwoWatches() :
		Source( getTemporaryDirectory() + "_xw_source" ),
		Destination( getTemporaryDirectory() + "_xw_destination" ),
		Watcher( useGeneric, 100 ) {
		removeDirectory( Source );
		removeDirectory( Destination );
		createDirectory( Source );
		createDirectory( Destination );
	}

	~TwoWatches() {
		Watcher.removeWatch( SourceId );
		Watcher.removeWatch( DestinationId );
		removeDirectory( Source );
		removeDirectory( Destination );
	}

	bool start( bool sourceRecursive, bool destinationRecursive ) {
		SourceId = Watcher.addWatch( Source, &SourceListener, sourceRecursive, crossMoveOptions );
		DestinationId = Watcher.addWatch( Destination, &DestinationListener,
										  destinationRecursive, crossMoveOptions );
		if ( SourceId <= 0 || DestinationId <= 0 )
			return false;
		Watcher.watch();
		createFile( Source + "/source_ready" );
		createFile( Destination + "/destination_ready" );
		bool ready = SourceListener.waitForActions( efsw::Actions::Add, "source_ready" ) &&
					 DestinationListener.waitForActions( efsw::Actions::Add, "destination_ready" );
		sleepMs( 200 );
		SourceListener.clearEvents();
		DestinationListener.clearEvents();
		return ready;
	}
};

// Polling backends compare directory snapshots and may never see a short-lived file.
bool pollsSnapshots() {
#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE
	return true;
#else
	return useGeneric;
#endif
}

// inotify and Windows report every create and delete. FSEvents can merge a create and a
// delete of one path into a single record, reported as Delete only.
bool reportsEveryCreate() {
#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY || EFSW_PLATFORM == EFSW_PLATFORM_WIN32
	return !useGeneric;
#else
	return false;
#endif
}

// A coalesced move must not also be reported through the Delete + Add fallback.
void expectSingleMove( TwoWatches& watches, const std::string& name, int* utest_result ) {
	EXPECT_EQ( -1, watches.SourceListener.indexOf( efsw::Actions::Delete, name ) );
	EXPECT_EQ( -1, watches.DestinationListener.indexOf( efsw::Actions::Add, name ) );
}

} // namespace

// A directory moved between two registrations must be owned by the destination afterwards:
// events inside it reach the destination listener and survive removing the source watch.
UTEST( CrossWatchMoves, DirectoryMoveHandsSubtreeToDestination ) {
	TwoWatches watches;
	EXPECT_TRUE( createDirectory( watches.Source + "/sub" ) );
	ASSERT_TRUE( watches.start( true, true ) );
	const std::string canonicalSource = efsw::FileSystem::getRealPath( watches.Source + "/sub" );

	EXPECT_TRUE( renameFile( watches.Source + "/sub", watches.Destination + "/sub" ) );
	EXPECT_TRUE( watches.DestinationListener.waitForActions( efsw::Actions::Moved, "sub" ) );
	EXPECT_TRUE(
		watches.DestinationListener.checkEvent( efsw::Actions::Moved, "sub", canonicalSource ) );
	sleepMs( 300 );
	expectSingleMove( watches, "sub", utest_result );

	EXPECT_TRUE( createFile( watches.Destination + "/sub/inside.txt", "x" ) );
	EXPECT_TRUE( watches.DestinationListener.waitForActions( efsw::Actions::Add, "inside.txt" ) );
	EXPECT_EQ( -1, watches.SourceListener.indexOf( efsw::Actions::Add, "inside.txt" ) );

	watches.Watcher.removeWatch( watches.SourceId );
	watches.SourceId = 0;
	sleepMs( 300 );
	EXPECT_TRUE( createFile( watches.Destination + "/sub/after_remove.txt", "x" ) );
	EXPECT_TRUE(
		watches.DestinationListener.waitForActions( efsw::Actions::Add, "after_remove.txt" ) );
}

// efsw-agent forwards a cross-watch Moved with an absolute old_path: the real event, converted
// as the agent does, is absolute and names the source.
UTEST( CrossWatchMoves, AgentKeepsAbsoluteSourceOfMoveFromAnotherWatch ) {
	TwoWatches watches;
	EXPECT_TRUE( createFile( watches.Source + "/agent.txt", "content" ) );
	ASSERT_TRUE( watches.start( true, true ) );
	const std::string canonicalSource = efsw::FileSystem::getRealPath( watches.Source + "/agent.txt" );

	EXPECT_TRUE( renameFile( watches.Source + "/agent.txt", watches.Destination + "/agent.txt" ) );
	ASSERT_TRUE( watches.DestinationListener.waitForActions( efsw::Actions::Moved, "agent.txt" ) );
	std::string oldFilename;
	for ( const auto& event : watches.DestinationListener.getEvents() )
		if ( std::get<0>( event ) == efsw::Actions::Moved && std::get<1>( event ) == "agent.txt" )
			oldFilename = std::get<2>( event );

	std::string root( watches.Destination );
	std::string oldPath( oldFilename );
	for ( std::string* path : { &root, &oldPath } )
		for ( char& c : *path )
			if ( c == '\\' )
				c = '/';
	root += '/';
	const std::string sent = efsw::agent::EventPath::fromRoot( root, "", oldPath );
	EXPECT_TRUE( efsw::agent::EventPath::isAbsolute( sent ) );
	EXPECT_TRUE( sent == oldPath );
	EXPECT_TRUE( oldFilename == canonicalSource );

	// The record goes over the wire in an EVENT_BATCH frame, as the agent sends it, and a
	// client parses it back with the absolute old_path.
	std::vector<efsw::agent::WatchEventRecord> records( 1 );
	records[0].action = efsw::agent::ActionMoved;
	records[0].path = efsw::agent::EventPath::fromRoot( root, "", root + "agent.txt" );
	records[0].oldPath = sent;
	efsw::agent::Writer writer;
	efsw::agent::EventBatch::write( writer, 7, records, 0, records.size() );
	std::vector<uint8_t> frame = writer.frame( efsw::agent::MessageEventBatch );

	efsw::agent::FrameParser parser;
	parser.feed( &frame[0], frame.size() );
	efsw::agent::Frame received;
	std::string error;
	ASSERT_EQ( 1, parser.next( received, error ) );
	EXPECT_EQ( efsw::agent::MessageEventBatch, received.type );
	efsw::agent::Reader reader( received.payload );
	uint32_t watchId = 0;
	std::vector<efsw::agent::WatchEventRecord> parsed;
	ASSERT_TRUE( efsw::agent::EventBatch::read( reader, watchId, parsed ) );
	EXPECT_EQ( 7u, watchId );
	ASSERT_EQ( 1u, parsed.size() );
	EXPECT_EQ( efsw::agent::ActionMoved, parsed[0].action );
	EXPECT_TRUE( parsed[0].path == "agent.txt" );
	EXPECT_TRUE( efsw::agent::EventPath::isAbsolute( parsed[0].oldPath ) );
	EXPECT_TRUE( parsed[0].oldPath == oldPath );
}

// The handed-over subtree belongs to the destination registration: removing the destination
// must also stop the events inside the moved directory, at every depth.
UTEST( CrossWatchMoves, RemovingDestinationRemovesMovedSubtree ) {
	TwoWatches watches;
	EXPECT_TRUE( createDirectory( watches.Source + "/sub" ) );
	EXPECT_TRUE( createDirectory( watches.Source + "/sub/deep" ) );
	ASSERT_TRUE( watches.start( true, true ) );

	EXPECT_TRUE( renameFile( watches.Source + "/sub", watches.Destination + "/sub" ) );
	EXPECT_TRUE( watches.DestinationListener.waitForActions( efsw::Actions::Moved, "sub" ) );
	EXPECT_TRUE( createFile( watches.Destination + "/sub/deep/before.txt", "x" ) );
	EXPECT_TRUE(
		watches.DestinationListener.waitForActions( efsw::Actions::Add, "before.txt" ) );

	watches.Watcher.removeWatch( watches.DestinationId );
	watches.DestinationId = 0;
	sleepMs( 300 );
	watches.SourceListener.clearEvents();
	watches.DestinationListener.clearEvents();
	EXPECT_TRUE( createFile( watches.Destination + "/sub/after.txt", "x" ) );
	EXPECT_TRUE( createFile( watches.Destination + "/sub/deep/after.txt", "x" ) );
	sleepMs( 500 );
	EXPECT_EQ( 0u, watches.DestinationListener.getEventCount() );
	EXPECT_EQ( 0u, watches.SourceListener.getEventCount() );
}

// Registrations are added and removed while directory trees move between them. Run under
// ThreadSanitizer and AddressSanitizer, this checks the watch bookkeeping of the event thread
// against the registration calls. The roots are not recursive, so the churned registrations
// below them are accepted by every backend, including inotify, which rejects registrations
// inside a recursive one.
UTEST( CrossWatchMoves, RegistrationChurnDuringMoves ) {
	TwoWatches watches;
	for ( int i = 0; i < 4; i++ ) {
		const std::string tree = watches.Source + "/tree" + std::to_string( i );
		EXPECT_TRUE( createDirectory( tree ) );
		EXPECT_TRUE( createDirectory( tree + "/a" ) );
		EXPECT_TRUE( createDirectory( tree + "/a/b" ) );
		EXPECT_TRUE( createDirectory( watches.Destination + "/dst" + std::to_string( i ) ) );
	}
	ASSERT_TRUE( watches.start( false, false ) );

	// Moves the subtree "a" between Source/treeN and Destination/dstN, which are the
	// registered directories.
	std::atomic<bool> stop{ false };
	std::thread mover( [&] {
		bool inSource = true;
		while ( !stop ) {
			for ( int i = 0; i < 4; i++ ) {
				const std::string source = watches.Source + "/tree" + std::to_string( i ) + "/a";
				const std::string destination =
					watches.Destination + "/dst" + std::to_string( i ) + "/a";
				const std::string& from = inSource ? source : destination;
				const std::string& to = inSource ? destination : source;
				renameFile( from, to );
				createFile( to + "/b/file.txt", "x" );
			}
			inSource = !inSource;
		}
	} );

	TestListener churnListener;
	int registered = 0;
	for ( int i = 0; i < 40; i++ ) {
		const std::string directory =
			i % 2 ? watches.Source + "/tree" + std::to_string( i % 4 )
				  : watches.Destination + "/dst" + std::to_string( i % 4 );
		const efsw::WatchID id =
			watches.Watcher.addWatch( directory, &churnListener, true, crossMoveOptions );
		sleepMs( 5 );
		if ( id > 0 ) {
			registered++;
			watches.Watcher.removeWatch( id );
		}
	}
	stop = true;
	mover.join();
	sleepMs( 300 );
	EXPECT_EQ( 40, registered );

	// The original registrations still work.
	EXPECT_TRUE( createFile( watches.Destination + "/final.txt", "x" ) );
	EXPECT_TRUE( watches.DestinationListener.waitForActions( efsw::Actions::Add, "final.txt" ) );
}

// A directory moved to another parent within one registration must be watched as part of its
// new parent: deleting the former parent must not stop its events, and deleting the new
// parent must remove it.
UTEST( CrossWatchMoves, MoveWithinRegistrationFollowsNewParent ) {
	const std::string root = getTemporaryDirectory() + "_xw_reparent";
	removeDirectory( root );
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( root + "/a" ) );
	EXPECT_TRUE( createDirectory( root + "/a/sub" ) );
	EXPECT_TRUE( createDirectory( root + "/b" ) );
	TestListener listener;
	{
		efsw::FileWatcher watcher( useGeneric, 100 );
		const efsw::WatchID id = watcher.addWatch( root, &listener, true, crossMoveOptions );
		ASSERT_GT( id, 0 );
		watcher.watch();
		sleepMs( 300 );

		EXPECT_TRUE( renameFile( root + "/a/sub", root + "/b/sub" ) );
		EXPECT_TRUE( listener.waitForActions( efsw::Actions::Moved, "sub" ) ||
					 listener.waitForActions( efsw::Actions::Add, "sub" ) );
		sleepMs( 300 );
		EXPECT_TRUE( createFile( root + "/b/sub/before.txt", "x" ) );
		EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "before.txt" ) );

		// The former parent goes away.
		EXPECT_TRUE( removeDirectory( root + "/a" ) );
		EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "a" ) );
		sleepMs( 300 );
		EXPECT_TRUE( createFile( root + "/b/sub/after.txt", "x" ) );
		EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "after.txt" ) );

		// The new parent goes away, then comes back: it is watched again.
		EXPECT_TRUE( removeDirectory( root + "/b" ) );
		EXPECT_TRUE( listener.waitForActions( efsw::Actions::Delete, "b" ) );
		sleepMs( 300 );
		EXPECT_TRUE( createDirectory( root + "/b" ) );
		EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "b" ) );
		sleepMs( 300 );
		EXPECT_TRUE( createFile( root + "/b/again.txt", "x" ) );
		EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "again.txt" ) );
		watcher.removeWatch( id );
	}
	removeDirectory( root );
}

// A listener removes its own registration on the first event of an action that reports
// several: inotify reports a file moved in from outside as Add then Modified. Nothing may be
// delivered after the removal.
UTEST( CrossWatchMoves, SelfRemovalStopsLaterCallbacksOfAnAction ) {
	const std::string root = getTemporaryDirectory() + "_xw_selfremove";
	const std::string outside = root + "_outside";
	removeDirectory( root );
	removeDirectory( outside );
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( outside ) );
	EXPECT_TRUE( createFile( outside + "/moved.txt", "x" ) );

	struct RemovingListener : TestListener {
		efsw::FileWatcher* Watcher{ NULL };
		std::atomic<efsw::WatchID> Id{ 0 };
		void handleFileAction( efsw::WatchID id, const std::string& dir, const std::string& file,
							   efsw::Action action, const std::string& old = "" ) override {
			TestListener::handleFileAction( id, dir, file, action, old );
			efsw::WatchID registered = Id.exchange( 0 );
			if ( registered > 0 )
				Watcher->removeWatch( registered );
		}
	} listener;
	{
		efsw::FileWatcher watcher( useGeneric, 100 );
		listener.Watcher = &watcher;
		const efsw::WatchID id = watcher.addWatch( root, &listener, true );
		ASSERT_GT( id, 0 );
		watcher.watch();
		sleepMs( 300 );
		listener.Id = id;

		EXPECT_TRUE( renameFile( outside + "/moved.txt", root + "/moved.txt" ) );
		EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "moved.txt" ) );
		sleepMs( 500 );
		EXPECT_EQ( 1u, listener.getEventCount() );
	}
	removeDirectory( root );
	removeDirectory( outside );
}

// A listener removes its registration from the first callback while a directory moved in is
// being discovered; on inotify, with synthetic events, that is the Add of a file inside it,
// before the watches of its subdirectories are installed. Nothing may be reported afterwards,
// from any directory of the moved tree.
UTEST( CrossWatchMoves, SelfRemovalDuringDiscoveryStopsTheTree ) {
	const std::string root = getTemporaryDirectory() + "_xw_discovery";
	const std::string outside = root + "_outside";
	removeDirectory( root );
	removeDirectory( outside );
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( outside ) );
	EXPECT_TRUE( createDirectory( outside + "/moved" ) );
	EXPECT_TRUE( createDirectory( outside + "/moved/nested" ) );
	EXPECT_TRUE( createDirectory( outside + "/moved/nested/deeper" ) );
	EXPECT_TRUE( createFile( outside + "/moved/seed.txt", "x" ) );

	struct RemovingListener : TestListener {
		efsw::FileWatcher* Watcher{ NULL };
		std::atomic<efsw::WatchID> Id{ 0 };
		void handleFileAction( efsw::WatchID id, const std::string& dir, const std::string& file,
							   efsw::Action action, const std::string& old = "" ) override {
			TestListener::handleFileAction( id, dir, file, action, old );
			efsw::WatchID registered = Id.exchange( 0 );
			if ( registered > 0 )
				Watcher->removeWatch( registered );
		}
	} listener;
	{
		efsw::FileWatcher watcher( useGeneric, 100 );
		listener.Watcher = &watcher;
		const efsw::WatchID id = watcher.addWatch(
			root, &listener, true, { { efsw::Options::LinuxProduceSyntheticEvents, 1 } } );
		ASSERT_GT( id, 0 );
		watcher.watch();
		sleepMs( 300 );
		listener.Id = id;

		EXPECT_TRUE( renameFile( outside + "/moved", root + "/moved" ) );
		for ( int i = 0; i < 200 && listener.getEventCount() == 0; i++ )
			sleepMs( 10 );
		EXPECT_EQ( 1u, listener.getEventCount() );
		sleepMs( 300 );
		EXPECT_TRUE( createFile( root + "/moved/nested/after.txt", "x" ) );
		EXPECT_TRUE( createFile( root + "/moved/nested/deeper/after.txt", "x" ) );
		sleepMs( 500 );
		EXPECT_EQ( 1u, listener.getEventCount() );
	}
	removeDirectory( root );
	removeDirectory( outside );
}

namespace {

/// Removes a registration when it receives its first event, then counts every callback it
/// gets afterwards: a listener may be destroyed as soon as its removal returns.
struct RemovalListener : efsw::FileWatchListener {
	efsw::FileWatcher* Watcher{ NULL };
	std::atomic<efsw::WatchID> RemoveOnFirstEvent{ 0 };
	std::atomic<bool> Removed{ false };
	std::atomic<int> Events{ 0 };
	std::atomic<int> EventsAfterRemoval{ 0 };

	void handleFileAction( efsw::WatchID, const std::string&, const std::string&, efsw::Action,
						   const std::string& ) override {
		Events++;
		if ( Removed )
			EventsAfterRemoval++;
		efsw::WatchID id = RemoveOnFirstEvent.exchange( 0 );
		if ( id > 0 ) {
			Watcher->removeWatch( id );
			Removed = true;
		}
	}
};

/// Plain events counter that knows when its registration was removed by someone else.
struct CountingListener : efsw::FileWatchListener {
	std::atomic<bool>* Removed{ NULL };
	std::atomic<int> EventsAfterRemoval{ 0 };

	void handleFileAction( efsw::WatchID, const std::string&, const std::string&, efsw::Action,
						   const std::string& ) override {
		if ( *Removed )
			EventsAfterRemoval++;
	}
};

void burst( const std::string& directory, const std::string& prefix ) {
	for ( int i = 0; i < 20; i++ ) {
		const std::string file = directory + "/" + prefix + std::to_string( i ) + ".txt";
		createFile( file, "x" );
		writeFile( file, "more" );
	}
}

} // namespace

// A listener removes its own registration on the first of many pending events: on every
// backend, with and without cross-directory moves (which make Windows hold Add events and
// flush them before later events), nothing more is delivered to it.
UTEST( CrossWatchMoves, SelfRemovalWithPendingEvents ) {
	for ( int option = 0; option < 2; option++ ) {
		const std::string root =
			getTemporaryDirectory() + "_xw_pending" + std::to_string( option );
		removeDirectory( root );
		EXPECT_TRUE( createDirectory( root ) );
		// Changes inside subdirectories make kqueue report the subdirectories as modified,
		// while it scans the rest of the tree.
		EXPECT_TRUE( createDirectory( root + "/sub0" ) );
		EXPECT_TRUE( createDirectory( root + "/sub1" ) );
		RemovalListener listener;
		{
			efsw::FileWatcher watcher( useGeneric, 100 );
			listener.Watcher = &watcher;
			std::vector<efsw::WatcherOption> options;
			if ( option )
				options = crossMoveOptions;
			const efsw::WatchID id = watcher.addWatch( root, &listener, true, options );
			ASSERT_GT( id, 0 );
			watcher.watch();
			sleepMs( 300 );
			listener.RemoveOnFirstEvent = id;

			burst( root + "/sub0", "pending" );
			burst( root, "pending" );
			burst( root + "/sub1", "pending" );
			for ( int i = 0; i < 200 && !listener.Removed; i++ )
				sleepMs( 10 );
			EXPECT_TRUE( listener.Removed.load() );
			burst( root + "/sub0", "later" );
			burst( root, "later" );
			sleepMs( 500 );
		}
		EXPECT_EQ( 0, listener.EventsAfterRemoval.load() );
		removeDirectory( root );
	}
}

// A listener removes another registration whose events are already part of the batch being
// delivered: nothing more is delivered to the removed registration.
UTEST( CrossWatchMoves, RemovingAnotherRegistrationWithPendingEvents ) {
	TwoWatches watches;
	// Not recursive, so that every backend accepts the registrations below the roots.
	ASSERT_TRUE( watches.start( false, false ) );
	RemovalListener remover;
	CountingListener removed;
	removed.Removed = &remover.Removed;
	remover.Watcher = &watches.Watcher;
	const std::string first = watches.Source + "/first";
	const std::string second = watches.Destination + "/second";
	EXPECT_TRUE( createDirectory( first ) );
	EXPECT_TRUE( createDirectory( second ) );
	sleepMs( 300 );
	const efsw::WatchID firstId =
		watches.Watcher.addWatch( first, &remover, true, crossMoveOptions );
	const efsw::WatchID secondId =
		watches.Watcher.addWatch( second, &removed, true, crossMoveOptions );
	ASSERT_GT( firstId, 0 );
	ASSERT_GT( secondId, 0 );
	sleepMs( 300 );
	remover.RemoveOnFirstEvent = secondId;

	for ( int i = 0; i < 20; i++ ) {
		createFile( first + "/a" + std::to_string( i ) + ".txt", "x" );
		createFile( second + "/b" + std::to_string( i ) + ".txt", "x" );
	}
	for ( int i = 0; i < 200 && !remover.Removed; i++ )
		sleepMs( 10 );
	EXPECT_TRUE( remover.Removed.load() );
	burst( second, "later" );
	sleepMs( 500 );
	watches.Watcher.removeWatch( firstId );
	EXPECT_EQ( 0, removed.EventsAfterRemoval.load() );
}

// Another thread removes a registration while its listener is inside a callback: removal
// returns only once the callback returned, since the caller may destroy the listener next.
// Another registration stays, so FSEvents replaces its stream instead of stopping it.
UTEST( CrossWatchMoves, RemoveWatchWaitsForRunningCallback ) {
	TwoWatches watches;
	ASSERT_TRUE( watches.start( true, true ) );

	struct SlowListener : efsw::FileWatchListener {
		std::atomic<bool> Entered{ false };
		std::atomic<bool> Returned{ false };
		void handleFileAction( efsw::WatchID, const std::string&, const std::string&,
							   efsw::Action, const std::string& ) override {
			if ( Entered.exchange( true ) )
				return;
			sleepMs( 300 );
			Returned = true;
		}
	} slow;
	const std::string directory = getTemporaryDirectory() + "_xw_slow";
	removeDirectory( directory );
	EXPECT_TRUE( createDirectory( directory ) );
	const efsw::WatchID id = watches.Watcher.addWatch( directory, &slow, true );
	ASSERT_GT( id, 0 );
	sleepMs( 300 );

	EXPECT_TRUE( createFile( directory + "/slow.txt", "x" ) );
	for ( int i = 0; i < 300 && !slow.Entered; i++ )
		sleepMs( 10 );
	ASSERT_TRUE( slow.Entered.load() );
	watches.Watcher.removeWatch( id );
	EXPECT_TRUE( slow.Returned.load() );
	removeDirectory( directory );
}

#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
namespace {
int openDescriptors() {
	int count = 0;
	for ( int fd = 0; fd < 4096; fd++ )
		if ( fcntl( fd, F_GETFD ) != -1 )
			count++;
	return count;
}
} // namespace

// Adding and removing a recursive watch over files and subdirectories, again and again,
// leaves no descriptor open: kqueue opens one per directory and per file.
UTEST( CrossWatchMoves, RepeatedAddRemoveKeepsDescriptors ) {
	const std::string root = getTemporaryDirectory() + "_xw_descriptors";
	removeDirectory( root );
	EXPECT_TRUE( createDirectory( root ) );
	for ( int d = 0; d < 3; d++ ) {
		const std::string sub = root + "/sub" + std::to_string( d );
		EXPECT_TRUE( createDirectory( sub ) );
		for ( int f = 0; f < 10; f++ )
			EXPECT_TRUE( createFile( sub + "/f" + std::to_string( f ), "x" ) );
	}
	for ( int f = 0; f < 10; f++ )
		EXPECT_TRUE( createFile( root + "/f" + std::to_string( f ), "x" ) );

	TestListener listener;
	efsw::FileWatcher watcher( useGeneric, 50 );
	watcher.watch();
	// The first registration may set up state that lives as long as the file watcher.
	watcher.removeWatch( watcher.addWatch( root, &listener, true ) );
	sleepMs( 200 );
	const int before = openDescriptors();
	for ( int i = 0; i < 20; i++ ) {
		const efsw::WatchID id = watcher.addWatch( root, &listener, true );
		EXPECT_GT( id, 0 );
		sleepMs( 20 );
		watcher.removeWatch( id );
	}
	sleepMs( 300 );
	EXPECT_EQ( before, openDescriptors() );
	removeDirectory( root );
}
#endif

// directories() runs while a directory holding a registration moves in and out of another
// registration, which renames the moved registration. Meant for ThreadSanitizer.
UTEST( CrossWatchMoves, DirectoriesDuringMovesOfRegistration ) {
	TwoWatches watches;
	EXPECT_TRUE( createDirectory( watches.Source + "/holder" ) );
	EXPECT_TRUE( createDirectory( watches.Source + "/holder/inner" ) );
	ASSERT_TRUE( watches.start( false, true ) );
	TestListener innerListener;
	const efsw::WatchID innerId = watches.Watcher.addWatch(
		watches.Source + "/holder/inner", &innerListener, true, crossMoveOptions );
	ASSERT_GT( innerId, 0 );

	std::atomic<bool> stop{ false };
	std::atomic<size_t> listed{ 0 };
	std::thread reader( [&] {
		while ( !stop )
			listed += watches.Watcher.directories().size();
	} );
	for ( int i = 0; i < 20; i++ ) {
		const bool out = i % 2 == 0;
		renameFile( ( out ? watches.Source : watches.Destination ) + "/holder",
					( out ? watches.Destination : watches.Source ) + "/holder" );
		sleepMs( 20 );
	}
	stop = true;
	reader.join();
	EXPECT_GT( listed.load(), 0u );
	watches.Watcher.removeWatch( innerId );
}

// A recursive destination must start covering a directory that arrives from a non-recursive
// source, which never had native watches inside it.
UTEST( CrossWatchMoves, DirectoryMoveFromNonRecursiveAddsCoverage ) {
	TwoWatches watches;
	EXPECT_TRUE( createDirectory( watches.Source + "/sub" ) );
	ASSERT_TRUE( watches.start( false, true ) );

	EXPECT_TRUE( renameFile( watches.Source + "/sub", watches.Destination + "/sub" ) );
	EXPECT_TRUE( watches.DestinationListener.waitForActions( efsw::Actions::Moved, "sub" ) );
	sleepMs( 300 );
	expectSingleMove( watches, "sub", utest_result );

	EXPECT_TRUE( createFile( watches.Destination + "/sub/inside.txt", "x" ) );
	EXPECT_TRUE( watches.DestinationListener.waitForActions( efsw::Actions::Add, "inside.txt" ) );
}

// A non-recursive destination must not report, and the source must no longer report, files
// inside a directory that left the recursive source tree.
UTEST( CrossWatchMoves, DirectoryMoveToNonRecursiveStopsSourceReports ) {
	TwoWatches watches;
	EXPECT_TRUE( createDirectory( watches.Source + "/sub" ) );
	ASSERT_TRUE( watches.start( true, false ) );

	EXPECT_TRUE( renameFile( watches.Source + "/sub", watches.Destination + "/sub" ) );
	EXPECT_TRUE( watches.DestinationListener.waitForActions( efsw::Actions::Moved, "sub" ) );
	sleepMs( 300 );
	expectSingleMove( watches, "sub", utest_result );

	EXPECT_TRUE( createFile( watches.Destination + "/sub/inside.txt", "x" ) );
	sleepMs( 500 );
	EXPECT_EQ( -1, watches.SourceListener.indexOf( efsw::Actions::Add, "inside.txt" ) );
}

// Enabling cross-directory moves must not reorder the events of ordinary file creation.
UTEST( CrossWatchMoves, CreateThenModifyKeepsOrder ) {
	TwoWatches watches;
	ASSERT_TRUE( watches.start( true, true ) );

	EXPECT_TRUE( createFile( watches.Source + "/written.txt", "content" ) );
	EXPECT_TRUE( watches.SourceListener.waitForActions( efsw::Actions::Add, "written.txt" ) );
	sleepMs( 300 );
	const int added = watches.SourceListener.indexOf( efsw::Actions::Add, "written.txt" );
	const int modified = watches.SourceListener.indexOf( efsw::Actions::Modified, "written.txt" );
	EXPECT_NE( -1, added );
	if ( modified != -1 )
		EXPECT_LT( added, modified );
}

// A move into a new directory must not be reported before the Add of that directory, even
// when the polling backends hold the destination's events for a pass because of an unrelated
// Delete. A directory is moved: entries already inside a new directory are only reported when
// they are directories, and only by the polling backends.
UTEST( CrossWatchMoves, MoveIntoNewDirectoryFollowsItsAdd ) {
	TwoWatches watches;
	EXPECT_TRUE( createDirectory( watches.Source + "/moved" ) );
	EXPECT_TRUE( createFile( watches.Destination + "/unrelated.txt", "content" ) );
	ASSERT_TRUE( watches.start( true, true ) );

	EXPECT_TRUE( removeFile( watches.Destination + "/unrelated.txt" ) );
	EXPECT_TRUE( createDirectory( watches.Destination + "/newparent" ) );
	EXPECT_TRUE( renameFile( watches.Source + "/moved", watches.Destination + "/newparent/moved" ) );
	EXPECT_TRUE( watches.DestinationListener.waitForActions( efsw::Actions::Add, "newparent" ) );
	EXPECT_TRUE(
		watches.DestinationListener.waitForActions( efsw::Actions::Delete, "unrelated.txt" ) );
	sleepMs( 500 );

	const int parent = watches.DestinationListener.indexOf( efsw::Actions::Add, "newparent" );
	int moved = watches.DestinationListener.indexOf( efsw::Actions::Moved, "moved" );
	if ( moved == -1 )
		moved = watches.DestinationListener.indexOf( efsw::Actions::Add, "moved" );
	if ( pollsSnapshots() )
		EXPECT_NE( -1, moved );
	if ( moved != -1 )
		EXPECT_LT( parent, moved );
}

UTEST( CrossWatchMoves, CreateThenDeleteKeepsOrder ) {
	TwoWatches watches;
	ASSERT_TRUE( watches.start( true, true ) );

	EXPECT_TRUE( createFile( watches.Source + "/temporary.txt", "content" ) );
	EXPECT_TRUE( removeFile( watches.Source + "/temporary.txt" ) );
	const bool deletedSeen =
		watches.SourceListener.waitForActions( efsw::Actions::Delete, "temporary.txt" );
	// Polling backends may never observe a file that lived between two scans.
	if ( !pollsSnapshots() )
		EXPECT_TRUE( deletedSeen );
	sleepMs( 300 );
	const int added = watches.SourceListener.indexOf( efsw::Actions::Add, "temporary.txt" );
	const int deleted = watches.SourceListener.indexOf( efsw::Actions::Delete, "temporary.txt" );
	if ( reportsEveryCreate() )
		EXPECT_NE( -1, added );
	if ( added != -1 ) {
		EXPECT_NE( -1, deleted );
		EXPECT_LT( added, deleted );
	}
}

namespace {

// A non-recursive watch on "source", an explicit watch on "source/sub/nested" and a recursive
// watch on "destination". Moving "source/sub" to "destination/sub" puts the explicit
// registration inside the recursive one, so one native directory has two logical owners.
struct NestedRegistrationMove {
	std::string Source;
	std::string Destination;
	TestListener SourceListener;
	TestListener NestedListener;
	TestListener DestinationListener;
	efsw::FileWatcher Watcher;
	efsw::WatchID SourceId{ 0 };
	efsw::WatchID NestedId{ 0 };
	efsw::WatchID DestinationId{ 0 };

	NestedRegistrationMove() :
		Source( getTemporaryDirectory() + "_xw_nested_source" ),
		Destination( getTemporaryDirectory() + "_xw_nested_destination" ),
		Watcher( useGeneric, 100 ) {
		removeDirectory( Source );
		removeDirectory( Destination );
		createDirectory( Source );
		createDirectory( Source + "/sub" );
		createDirectory( Source + "/sub/nested" );
		createDirectory( Destination );
	}

	~NestedRegistrationMove() {
		Watcher.removeWatch( SourceId );
		Watcher.removeWatch( NestedId );
		Watcher.removeWatch( DestinationId );
		removeDirectory( Source );
		removeDirectory( Destination );
	}

	enum Setup { Ready, MoveRefused, Failed };

	Setup startAndMove() {
		SourceId = Watcher.addWatch( Source, &SourceListener, false, crossMoveOptions );
		NestedId = Watcher.addWatch( Source + "/sub/nested", &NestedListener, false,
									 crossMoveOptions );
		DestinationId =
			Watcher.addWatch( Destination, &DestinationListener, true, crossMoveOptions );
		if ( SourceId <= 0 || NestedId <= 0 || DestinationId <= 0 )
			return Failed;
		Watcher.watch();
		createFile( Source + "/sub/nested/nested_ready" );
		createFile( Destination + "/destination_ready" );
		if ( !NestedListener.waitForActions( efsw::Actions::Add, "nested_ready" ) ||
			 !DestinationListener.waitForActions( efsw::Actions::Add, "destination_ready" ) )
			return Failed;
		sleepMs( 200 );

		// Windows refuses to rename a directory while a directory inside it is open.
		if ( !renameFile( Source + "/sub", Destination + "/sub" ) )
			return MoveRefused;
		if ( !DestinationListener.waitForActions( efsw::Actions::Moved, "sub" ) )
			return Failed;
		sleepMs( 300 );
		// The move must be reported once, not also through the Delete + Add fallback.
		if ( -1 != SourceListener.indexOf( efsw::Actions::Delete, "sub" ) ||
			 -1 != DestinationListener.indexOf( efsw::Actions::Add, "sub" ) )
			return Failed;
		SourceListener.clearEvents();
		NestedListener.clearEvents();
		DestinationListener.clearEvents();
		return Ready;
	}
};

// inotify watches the directory itself, so a registration keeps working after its directory
// is moved. The other backends watch paths, and lose a registration whose path moved away.
bool followsMovedRegistrations() {
#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY
	return !useGeneric;
#else
	return false;
#endif
}

#define START_NESTED_REGISTRATION_MOVE( watches )                                               \
	do {                                                                                         \
		const NestedRegistrationMove::Setup setup = ( watches ).startAndMove();                  \
		if ( setup == NestedRegistrationMove::MoveRefused )                                      \
			UTEST_SKIP( "the platform refuses to move a directory containing a watched one" );   \
		ASSERT_EQ( NestedRegistrationMove::Ready, setup );                                       \
	} while ( 0 )

} // namespace

// Both logical owners of the moved native directory keep receiving its events.
UTEST( CrossWatchMoves, MovedExplicitRegistrationKeepsReceivingEvents ) {
	NestedRegistrationMove watches;
	START_NESTED_REGISTRATION_MOVE( watches );

	EXPECT_TRUE( createFile( watches.Destination + "/sub/nested/after.txt", "x" ) );
	EXPECT_TRUE( watches.DestinationListener.waitForActions( efsw::Actions::Add, "after.txt" ) );
	if ( followsMovedRegistrations() )
		EXPECT_TRUE( watches.NestedListener.waitForActions( efsw::Actions::Add, "after.txt" ) );
	EXPECT_EQ( -1, watches.SourceListener.indexOf( efsw::Actions::Add, "after.txt" ) );
}

// Removing the explicit registration must not disable the recursive destination there.
UTEST( CrossWatchMoves, RemovingMovedExplicitRegistrationKeepsDestination ) {
	NestedRegistrationMove watches;
	START_NESTED_REGISTRATION_MOVE( watches );

	watches.Watcher.removeWatch( watches.NestedId );
	watches.NestedId = 0;
	sleepMs( 300 );
	EXPECT_TRUE( createFile( watches.Destination + "/sub/nested/after.txt", "x" ) );
	EXPECT_TRUE( watches.DestinationListener.waitForActions( efsw::Actions::Add, "after.txt" ) );
	EXPECT_EQ( -1, watches.NestedListener.indexOf( efsw::Actions::Add, "after.txt" ) );
}

// Removing the recursive destination must not disable the explicit registration.
UTEST( CrossWatchMoves, RemovingDestinationKeepsMovedExplicitRegistration ) {
	if ( !followsMovedRegistrations() )
		UTEST_SKIP( "the backend watches paths and cannot follow a moved registration" );
	NestedRegistrationMove watches;
	START_NESTED_REGISTRATION_MOVE( watches );

	watches.Watcher.removeWatch( watches.DestinationId );
	watches.DestinationId = 0;
	sleepMs( 300 );
	EXPECT_TRUE( createFile( watches.Destination + "/sub/nested/after.txt", "x" ) );
	EXPECT_TRUE( watches.NestedListener.waitForActions( efsw::Actions::Add, "after.txt" ) );
	EXPECT_EQ( -1, watches.DestinationListener.indexOf( efsw::Actions::Add, "after.txt" ) );
}

// When registrations are nested, a move inside the outer watch out of the inner one must still
// tell the inner watch that its file is gone.
UTEST( CrossWatchMoves, NestedWatchStillSeesItsHalf ) {
	const std::string outer = getTemporaryDirectory() + "_xw_outer";
	removeDirectory( outer );
	EXPECT_TRUE( createDirectory( outer ) );
	EXPECT_TRUE( createDirectory( outer + "/inner" ) );
	EXPECT_TRUE( createDirectory( outer + "/other" ) );

	TestListener outerListener;
	TestListener innerListener;
	efsw::FileWatcher watcher( useGeneric, 100 );
	const efsw::WatchID outerId =
		watcher.addWatch( outer, &outerListener, true, crossMoveOptions );
	ASSERT_GT( outerId, 0 );
	const efsw::WatchID innerId =
		watcher.addWatch( outer + "/inner", &innerListener, true, crossMoveOptions );
	if ( innerId <= 0 ) {
		// Backends that reject overlapping registrations have nothing to verify.
		removeDirectory( outer );
		return;
	}

	watcher.watch();
	EXPECT_TRUE( createFile( outer + "/inner/file.txt", "x" ) );
	EXPECT_TRUE( innerListener.waitForActions( efsw::Actions::Add, "file.txt" ) );
	sleepMs( 300 );
	innerListener.clearEvents();
	outerListener.clearEvents();

	EXPECT_TRUE( renameFile( outer + "/inner/file.txt", outer + "/other/file.txt" ) );
	EXPECT_TRUE( innerListener.waitForActions( efsw::Actions::Delete, "file.txt" ) );
	EXPECT_TRUE( outerListener.waitForActions( efsw::Actions::Moved, "file.txt" ) ||
				 outerListener.waitForActions( efsw::Actions::Add, "file.txt" ) );

	watcher.removeWatch( innerId );
	watcher.removeWatch( outerId );
	removeDirectory( outer );
}

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE
#include <efsw/FileWatcherKqueue.hpp>
#include <efsw/System.hpp>
#include <efsw/WatcherKqueue.hpp>
#include <efsw/FileActionBatch.hpp>
#include <efsw/FileInfo.hpp>
#include <mutex>
#include <sys/time.h>

namespace {

/// A kqueue file watcher that can pretend its file descriptors ran out, so that new
/// subdirectory watches fall back to the generic watcher.
struct ExhaustibleKqueue : efsw::FileWatcherKqueue {
	using efsw::FileWatcherKqueue::FileWatcherKqueue;

	void exhaust() { mFileDescriptorCount = static_cast<long>( efsw::System::getMaxFD() ); }

	long descriptors() const { return mFileDescriptorCount.load(); }
};

} // namespace

// Registrations added and removed from two threads at once, with the file descriptors used up
// so that every watch falls back to the generic watcher. Meant for ThreadSanitizer.
UTEST( CrossWatchMoves, KqueueConcurrentFallbackRegistrations ) {
	const std::string root = getTemporaryDirectory() + "_xw_kqconcurrent";
	removeDirectory( root );
	EXPECT_TRUE( createDirectory( root ) );
	for ( int i = 0; i < 2; i++ ) {
		EXPECT_TRUE( createDirectory( root + "/t" + std::to_string( i ) ) );
		EXPECT_TRUE( createDirectory( root + "/t" + std::to_string( i ) + "/sub" ) );
	}
	TestListener listener;
	efsw::FileWatcher owner( true );
	{
		ExhaustibleKqueue kqueue( &owner, 20 );
		kqueue.exhaust();
		kqueue.watch();
		std::atomic<int> registered{ 0 };
		auto churn = [&]( int tree ) {
			const std::string directory = root + "/t" + std::to_string( tree );
			for ( int i = 0; i < 20; i++ ) {
				efsw::WatchID id = kqueue.addWatch( directory, &listener, true, {} );
				if ( id > 0 ) {
					registered++;
					createFile( directory + "/sub/f" + std::to_string( i ), "x" );
					sleepMs( 5 );
					kqueue.removeWatch( id );
				}
			}
		};
		std::thread first( churn, 0 );
		std::thread second( churn, 1 );
		first.join();
		second.join();
		EXPECT_EQ( 40, registered.load() );
	}
	removeDirectory( root );
}

// The file watcher counts the descriptors its watches hold, to fall back to the generic
// watcher before running out: removing a watch must give all of them back.
UTEST( CrossWatchMoves, KqueueDescriptorCountReturnsAfterRemoval ) {
	const std::string root = getTemporaryDirectory() + "_xw_kqcount";
	removeDirectory( root );
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( root + "/sub" ) );
	for ( int f = 0; f < 5; f++ ) {
		EXPECT_TRUE( createFile( root + "/f" + std::to_string( f ), "x" ) );
		EXPECT_TRUE( createFile( root + "/sub/f" + std::to_string( f ), "x" ) );
	}
	TestListener listener;
	efsw::FileWatcher owner( true );
	{
		ExhaustibleKqueue kqueue( &owner, 20 );
		const long before = kqueue.descriptors();
		for ( int i = 0; i < 10; i++ ) {
			const efsw::WatchID id = kqueue.addWatch( root, &listener, true, {} );
			EXPECT_GT( id, 0 );
			EXPECT_GT( kqueue.descriptors(), before );
			kqueue.removeWatch( id );
			EXPECT_EQ( before, kqueue.descriptors() );
		}
	}
	removeDirectory( root );
}

// A generic watch that replaces a kqueue subdirectory watch, when file descriptors ran out,
// reports into the root's shared batch as part of the root's registration: its events wait for
// the end of the pass, where moves to and from other registrations are paired, and removing the
// root cancels them.
UTEST( CrossWatchMoves, KqueueGenericFallbackReportsIntoTheSharedBatch ) {
	const std::string root = getTemporaryDirectory() + "_xw_kqshared";
	removeDirectory( root );
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( root + "/sub" ) );
	TestListener listener;
	efsw::FileWatcher owner( true );
	{
		ExhaustibleKqueue kqueue( &owner, 100 );
		efsw::FileActionBatch batch;
		efsw::WatcherKqueue watch( 1, root, &listener, true, &kqueue, NULL, true );
		watch.SharedActionBatch = &batch;
		kqueue.exhaust();
		watch.addAll();

		EXPECT_TRUE( createFile( root + "/sub/shared.txt", "x" ) );
		batch.beginPass();
		watch.watch();
		EXPECT_EQ( 0u, listener.getEventCount() );
		batch.scanned( &watch );
		batch.finishPass();
		// Some file systems also report the subdirectory as modified.
		EXPECT_NE( -1, listener.indexOf( efsw::Actions::Add, "shared.txt" ) );

		listener.clearEvents();
		EXPECT_TRUE( createFile( root + "/sub/canceled.txt", "x" ) );
		batch.beginPass();
		watch.watch();
		batch.forget( &watch );
		batch.scanned( &watch );
		batch.finishPass();
		EXPECT_EQ( 0u, listener.getEventCount() );
	}
	removeDirectory( root );
}

// A subdirectory watch reports its own directory's change to its parent, during the root's scan
// and after other events of the tree were collected: the report joins the shared batch, in
// order, instead of running a listener in the middle of the scan, and removing the root cancels
// it with the rest.
UTEST( CrossWatchMoves, KqueueDirectoryChangeWaitsForTheRootsPass ) {
	const std::string root = getTemporaryDirectory() + "_xw_kqdirchanged";
	removeDirectory( root );
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( root + "/sub" ) );
	EXPECT_TRUE( createDirectory( root + "/sub/inner" ) );
	TestListener listener;
	efsw::FileWatcher owner( true );
	{
		ExhaustibleKqueue kqueue( &owner, 100 );
		efsw::FileActionBatch batch;
		efsw::WatcherKqueue watch( 1, root, &listener, true, &kqueue, NULL, true );
		watch.SharedActionBatch = &batch;
		watch.addAll();

		for ( int cancel = 0; cancel < 2; cancel++ ) {
			listener.clearEvents();
			const std::string tag = std::to_string( cancel );
			EXPECT_TRUE( createFile( root + "/sub/inner/a" + tag + ".txt", "x" ) );
			EXPECT_TRUE( createFile( root + "/sub/b" + tag + ".txt", "x" ) );
			// Times unlike the snapshots', whatever the file system's resolution, so that both
			// directories report a change. The deeper one is scanned first.
			struct timeval times[2] = { { 1000000000 + cancel, 0 }, { 1000000000 + cancel, 0 } };
			EXPECT_EQ( 0, utimes( ( root + "/sub/inner" ).c_str(), times ) );
			EXPECT_EQ( 0, utimes( ( root + "/sub" ).c_str(), times ) );

			batch.beginPass();
			watch.watch();
			EXPECT_EQ( 0u, listener.getEventCount() );
			if ( cancel )
				batch.forget( &watch );
			batch.scanned( &watch );
			batch.finishPass();
			if ( cancel ) {
				EXPECT_EQ( 0u, listener.getEventCount() );
			} else {
				const int added = listener.indexOf( efsw::Actions::Add, "a0.txt" );
				EXPECT_NE( -1, added );
				EXPECT_GT( listener.indexOf( efsw::Actions::Modified, "sub" ), added );
			}
		}
	}
	removeDirectory( root );
}

// A file moves between a generic watch, which replaced a kqueue subdirectory watch when file
// descriptors ran out, and another registration, both ways: each move is paired in the shared
// batch and reported as a Moved event to its destination only.
UTEST( CrossWatchMoves, KqueueGenericFallbackMovesToAnotherRegistration ) {
	if ( !efsw::FileInfo::inodeSupported() ) {
		UTEST_SKIP( "File identity is not supported." );
	}
	const std::string base = getTemporaryDirectory() + "_xw_kqfallbackmove";
	const std::string first = base + "/first";
	const std::string second = base + "/second";
	removeDirectory( base );
	EXPECT_TRUE( createDirectory( base ) );
	EXPECT_TRUE( createDirectory( first ) );
	EXPECT_TRUE( createDirectory( first + "/sub" ) );
	EXPECT_TRUE( createDirectory( second ) );
	EXPECT_TRUE( createFile( first + "/sub/out.txt", "x" ) );
	EXPECT_TRUE( createFile( second + "/in.txt", "x" ) );
	TestListener firstListener;
	TestListener secondListener;
	efsw::FileWatcher owner( true );
	{
		ExhaustibleKqueue kqueue( &owner, 100 );
		efsw::FileActionBatch batch;
		efsw::WatcherKqueue firstWatch( 1, first, &firstListener, true, &kqueue, NULL, true );
		efsw::WatcherKqueue secondWatch( 2, second, &secondListener, true, &kqueue, NULL, true );
		firstWatch.SharedActionBatch = &batch;
		secondWatch.SharedActionBatch = &batch;
		secondWatch.addAll();
		kqueue.exhaust();
		firstWatch.addAll();

		auto pass = [&]() {
			batch.beginPass();
			firstWatch.watch();
			batch.scanned( &firstWatch );
			secondWatch.watch();
			batch.scanned( &secondWatch );
			batch.finishPass();
		};

		EXPECT_TRUE( renameFile( first + "/sub/out.txt", second + "/out.txt" ) );
		pass();
		EXPECT_NE( -1, secondListener.indexOf( efsw::Actions::Moved, "out.txt" ) );
		EXPECT_EQ( -1, firstListener.indexOf( efsw::Actions::Delete, "out.txt" ) );
		EXPECT_EQ( -1, secondListener.indexOf( efsw::Actions::Add, "out.txt" ) );

		firstListener.clearEvents();
		secondListener.clearEvents();
		EXPECT_TRUE( renameFile( second + "/in.txt", first + "/sub/in.txt" ) );
		pass();
		// A Delete collected after the destination's scan may be held to the next pass.
		pass();
		EXPECT_NE( -1, firstListener.indexOf( efsw::Actions::Moved, "in.txt" ) );
		EXPECT_EQ( -1, firstListener.indexOf( efsw::Actions::Add, "in.txt" ) );
		EXPECT_EQ( -1, secondListener.indexOf( efsw::Actions::Delete, "in.txt" ) );
	}
	removeDirectory( base );
}

// A directory watched by generic watches, because file descriptors ran out, is renamed inside
// a kqueue watch: the generic watches follow it, and report from its new path.
UTEST( CrossWatchMoves, KqueueMoveDirectoryWithGenericFallbackWatches ) {
	const std::string root = getTemporaryDirectory() + "_xw_kqmove";
	removeDirectory( root );
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( root + "/sub" ) );
	EXPECT_TRUE( createDirectory( root + "/sub/inner" ) );
	struct DirectoryListener : TestListener {
		std::mutex Mutex;
		std::vector<std::string> Directories;
		void handleFileAction( efsw::WatchID id, const std::string& dir, const std::string& file,
							   efsw::Action action, const std::string& old = "" ) override {
			{
				std::lock_guard<std::mutex> lock( Mutex );
				Directories.push_back( dir + file );
			}
			TestListener::handleFileAction( id, dir, file, action, old );
		}
	} listener;
	efsw::FileWatcher owner( true );
	{
		ExhaustibleKqueue kqueue( &owner, 100 );
		efsw::WatcherKqueue watch( 1, root, &listener, true, &kqueue );
		kqueue.exhaust();
		watch.addAll();

		EXPECT_TRUE( renameFile( root + "/sub", root + "/moved" ) );
		watch.watch();
		watch.watch();
		listener.clearEvents();
		EXPECT_TRUE( createFile( root + "/moved/inner/after.txt", "x" ) );
		watch.watch();
		EXPECT_NE( -1, listener.indexOf( efsw::Actions::Add, "after.txt" ) );
		bool reportedFromNewPath = false;
		{
			std::lock_guard<std::mutex> lock( listener.Mutex );
			for ( const std::string& path : listener.Directories )
				if ( path.find( "/moved/inner/after.txt" ) != std::string::npos )
					reportedFromNewPath = true;
		}
		EXPECT_TRUE( reportedFromNewPath );
	}
	removeDirectory( root );
}

// Retiring a kqueue watch must retire its generic fallback descendants, which report on their
// own: after retirement, nothing is reported from them.
UTEST( CrossWatchMoves, KqueueRetireReachesGenericFallbackWatches ) {
	const std::string root = getTemporaryDirectory() + "_xw_kqretire";
	removeDirectory( root );
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( root + "/sub" ) );
	TestListener listener;
	efsw::FileWatcher owner( true );
	{
		ExhaustibleKqueue kqueue( &owner, 100 );
		efsw::WatcherKqueue watch( 1, root, &listener, true, &kqueue );
		kqueue.exhaust();
		watch.addAll();

		// The fallback watch reports on its own while the root is active.
		EXPECT_TRUE( createFile( root + "/sub/before.txt", "x" ) );
		watch.watch();
		EXPECT_NE( -1, listener.indexOf( efsw::Actions::Add, "before.txt" ) );

		watch.retire();
		listener.clearEvents();
		EXPECT_TRUE( createFile( root + "/sub/after.txt", "x" ) );
		watch.watch();
		EXPECT_EQ( 0u, listener.getEventCount() );
	}
	removeDirectory( root );
}
#endif
