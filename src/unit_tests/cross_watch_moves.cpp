#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileSystem.hpp>
#include <efsw/base.hpp>

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
