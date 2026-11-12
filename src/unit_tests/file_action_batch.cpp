#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileActionBatch.hpp>
#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>

using namespace efsw_test;

namespace {

/// A watch of a polling backend whose scans report scripted events.
class FakeWatch : public efsw::Watcher {
  public:
	struct Pending {
		std::string Directory;
		std::string Filename;
		efsw::Action Action;
		efsw::FileInfo Info;
	};

	efsw::FileActionBatch& Batch;
	/// Identities listed in the watch's snapshot.
	std::set<efsw::FileIdentity> Snapshot;
	/// Events reported by the next scan.
	std::vector<Pending> Next;
	int Scans{ 0 };

	FakeWatch( efsw::WatchID id, efsw::FileActionBatch& batch, efsw::FileWatchListener* listener ) :
		Watcher( id, "", listener, true ), Batch( batch ) {}

	bool collectsSharedActions() const override { return true; }

	bool snapshotHasAny( const std::set<efsw::FileIdentity>& identities ) const override {
		for ( const efsw::FileIdentity& identity : Snapshot )
			if ( identities.count( identity ) )
				return true;
		return false;
	}

	void watch() override {
		Scans++;
		for ( const Pending& event : Next )
			Batch.add( ID, event.Directory, event.Filename, event.Action, "", event.Info, Listener,
					   this );
		Next.clear();
	}

	void report( const std::string& directory, const std::string& filename, efsw::Action action,
				 efsw::FileInfo info ) {
		info.Filepath = directory + filename;
		// Paths use the platform separator, as the backends report them.
		for ( char& c : info.Filepath )
			if ( c == '/' )
				c = efsw::FileSystem::getOSSlash();
		Next.push_back( { directory, filename, action, info } );
	}
};

/// A real file, so pairing sees a complete filesystem identity.
struct MovedFile {
	std::string Directory;
	efsw::FileInfo Info;
	efsw::FileIdentity Identity;

	bool create() {
		Directory = getTemporaryDirectory();
		if ( !createDirectory( Directory ) || !createFile( Directory + "/file.txt", "content" ) )
			return false;
		Info = efsw::FileInfo( Directory + "/file.txt" );
		Identity = Info.identity();
		return Info.Inode != 0;
	}

	/// Creates another file, or a directory, with its own identity.
	efsw::FileInfo make( const std::string& name, bool directory = false ) {
		std::string path( Directory + "/" + name );
		if ( directory )
			createDirectory( path );
		else
			createFile( path, "content" );
		return efsw::FileInfo( path );
	}

	~MovedFile() {
		if ( !Directory.empty() )
			removeDirectory( Directory );
	}
};

void runPass( efsw::FileActionBatch& batch, const std::vector<FakeWatch*>& watches ) {
	batch.beginPass();
	for ( FakeWatch* watch : watches ) {
		watch->watch();
		batch.scanned( watch );
	}
	batch.finishPass();
}

} // namespace

#define START_BATCH_TEST()                                \
	if ( !efsw::FileInfo::inodeSupported() ) {            \
		UTEST_SKIP( "File identity is not supported." );  \
	}                                                     \
	MovedFile file;                                       \
	ASSERT_TRUE( file.create() );                         \
	efsw::FileActionBatch batch;                          \
	TestListener listener;                                \
	FakeWatch first( 1, batch, &listener );               \
	FakeWatch second( 2, batch, &listener );              \
	FakeWatch third( 3, batch, &listener );               \
	std::vector<FakeWatch*> watches{ &first, &second, &third }

/// Removes a watch from the batch when it receives its first event, as a listener that
/// removes a registration, possibly its own, would.
class RemovingListener : public TestListener {
  public:
	efsw::FileActionBatch* Batch{ NULL };
	efsw::Watcher* Removed{ NULL };

	void handleFileAction( efsw::WatchID id, const std::string& dir, const std::string& filename,
						   efsw::Action action, const std::string& oldFilename = "" ) override {
		TestListener::handleFileAction( id, dir, filename, action, oldFilename );
		if ( Removed ) {
			Batch->forget( Removed );
			Removed = NULL;
		}
	}
};

// The file moved from the first watch to the third after the first was scanned: its snapshot
// still lists the file, so only the first watch is scanned again, and finds the Delete.
UTEST( FileActionBatch, SplitAddRescansOnlyTheSourceWatch ) {
	START_BATCH_TEST();
	first.Snapshot.insert( file.Identity );
	third.report( "/dst/", "file.txt", efsw::Actions::Add, file.Info );

	batch.beginPass();
	first.watch();
	batch.scanned( &first );
	// The rename happens here: the next scan of the first watch reports the Delete.
	first.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );
	second.watch();
	batch.scanned( &second );
	third.watch();
	batch.scanned( &third );
	batch.finishPass();

	EXPECT_EQ( 2, first.Scans );
	EXPECT_EQ( 1, second.Scans );
	EXPECT_EQ( 1, third.Scans );
	EXPECT_EQ( 1u, listener.getEventCount() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Moved, "file.txt" ) );
}

// A watch scanned after the one that collected the Add saw the file already moved, so it is
// not scanned again even if its snapshot lists the identity.
UTEST( FileActionBatch, SplitAddDoesNotRescanLaterWatches ) {
	START_BATCH_TEST();
	third.Snapshot.insert( file.Identity );
	first.report( "/dst/", "file.txt", efsw::Actions::Add, file.Info );

	runPass( batch, watches );

	EXPECT_EQ( 1, first.Scans );
	EXPECT_EQ( 1, second.Scans );
	EXPECT_EQ( 1, third.Scans );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Add, "file.txt" ) );
}

// A created file is listed in no snapshot: no watch is scanned again.
UTEST( FileActionBatch, OrdinaryAddDoesNotRescan ) {
	START_BATCH_TEST();
	third.report( "/dst/", "file.txt", efsw::Actions::Add, file.Info );

	runPass( batch, watches );

	EXPECT_EQ( 1, first.Scans );
	EXPECT_EQ( 1, second.Scans );
	EXPECT_EQ( 1, third.Scans );
	EXPECT_EQ( 1u, listener.getEventCount() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Add, "file.txt" ) );
}

// The file moved from the third watch to the first after the first was scanned: the Delete
// waits for the next pass, where the first watch reports the Add. Later events of the third
// watch wait with it, and nothing is scanned again.
UTEST( FileActionBatch, SplitDeleteWaitsForTheNextPass ) {
	START_BATCH_TEST();
	third.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );
	third.report( "/src/", "later.txt", efsw::Actions::Modified, efsw::FileInfo() );
	second.report( "/other/", "other.txt", efsw::Actions::Modified, efsw::FileInfo() );

	runPass( batch, watches );

	EXPECT_EQ( 1u, listener.getEventCount() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Modified, "other.txt" ) );

	first.report( "/dst/", "file.txt", efsw::Actions::Add, file.Info );
	runPass( batch, watches );

	EXPECT_EQ( 2, first.Scans );
	EXPECT_EQ( 2, second.Scans );
	EXPECT_EQ( 2, third.Scans );
	EXPECT_EQ( 3u, listener.getEventCount() );
	// The later events of the source watch wait until its move is resolved.
	EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Moved, "file.txt" ) );
	EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Modified, "later.txt" ) );
	EXPECT_EQ( -1, listener.indexOf( efsw::Actions::Delete, "file.txt" ) );
}

// A deleted file waits one pass, then is reported, and is not held again.
UTEST( FileActionBatch, OrdinaryDeleteIsDeliveredOnTheNextPass ) {
	START_BATCH_TEST();
	third.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );

	runPass( batch, watches );
	EXPECT_EQ( 0u, listener.getEventCount() );

	runPass( batch, watches );
	EXPECT_EQ( 1u, listener.getEventCount() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Delete, "file.txt" ) );
	EXPECT_EQ( 2, first.Scans );
}

// No watch was scanned before the first one, so its Delete cannot be split.
UTEST( FileActionBatch, DeleteInFirstWatchIsNotHeld ) {
	START_BATCH_TEST();
	first.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );

	runPass( batch, watches );

	EXPECT_EQ( 1u, listener.getEventCount() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Delete, "file.txt" ) );
}

// A removed watch's held events are dropped, its listener may be gone.
UTEST( FileActionBatch, ForgetDropsHeldEvents ) {
	START_BATCH_TEST();
	third.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );

	runPass( batch, watches );
	batch.forget( &third );
	runPass( batch, { &first, &second } );

	EXPECT_EQ( 0u, listener.getEventCount() );
}

// An unrelated Delete is held, but the halves of a complete move after it are not: the move is
// reported in its pass.
UTEST( FileActionBatch, HoldKeepsLaterCompleteMove ) {
	START_BATCH_TEST();
	efsw::FileInfo unrelated = file.make( "unrelated.txt" );
	first.report( "/dst/", "file.txt", efsw::Actions::Add, file.Info );
	third.report( "/src/", "unrelated.txt", efsw::Actions::Delete, unrelated );
	third.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );

	runPass( batch, watches );
	EXPECT_EQ( 1u, listener.getEventCount() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Moved, "file.txt" ) );

	runPass( batch, watches );
	EXPECT_EQ( 2u, listener.getEventCount() );
	EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Delete, "unrelated.txt" ) );
	EXPECT_EQ( -1, listener.indexOf( efsw::Actions::Delete, "file.txt" ) );
}

UTEST( FileActionBatch, HoldKeepsEarlierCompleteMove ) {
	START_BATCH_TEST();
	efsw::FileInfo unrelated = file.make( "unrelated.txt" );
	first.report( "/dst/", "file.txt", efsw::Actions::Add, file.Info );
	third.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );
	third.report( "/src/", "unrelated.txt", efsw::Actions::Delete, unrelated );

	runPass( batch, watches );
	EXPECT_EQ( 1u, listener.getEventCount() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Moved, "file.txt" ) );

	runPass( batch, watches );
	EXPECT_EQ( 2u, listener.getEventCount() );
	EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Delete, "unrelated.txt" ) );
}

// The events inside a moved directory belong to its Moved event, and are not held either.
UTEST( FileActionBatch, HoldKeepsCompleteDirectoryMove ) {
	START_BATCH_TEST();
	efsw::FileInfo unrelated = file.make( "unrelated.txt" );
	efsw::FileInfo directory = file.make( "dir", true );
	efsw::FileInfo child = file.make( "dir/child.txt" );
	// As in the generic backend: the destination snapshots the directory's contents silently,
	// and the source reports the Delete of each of its entries.
	first.report( "/dst/", "dir", efsw::Actions::Add, directory );
	third.report( "/src/", "unrelated.txt", efsw::Actions::Delete, unrelated );
	third.report( "/src/", "dir", efsw::Actions::Delete, directory );
	third.report( "/src/dir/", "child.txt", efsw::Actions::Delete, child );

	runPass( batch, watches );
	EXPECT_EQ( 1u, listener.getEventCount() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Moved, "dir" ) );

	runPass( batch, watches );
	EXPECT_EQ( 2u, listener.getEventCount() );
	EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Delete, "unrelated.txt" ) );
	EXPECT_EQ( -1, listener.indexOf( efsw::Actions::Delete, "child.txt" ) );
}

// A complete move whose destination comes after a held event of its watch is held with it:
// its Moved event must not overtake the Add of its new parent directory.
UTEST( FileActionBatch, HeldDestinationHoldsCompleteMove ) {
	START_BATCH_TEST();
	TestListener secondListener;
	second.Listener = &secondListener;
	efsw::FileInfo unrelated = file.make( "unrelated.txt" );
	efsw::FileInfo parent = file.make( "newparent", true );
	first.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );
	second.report( "/dst/", "unrelated.txt", efsw::Actions::Delete, unrelated );
	second.report( "/dst/", "newparent", efsw::Actions::Add, parent );
	second.report( "/dst/newparent/", "file.txt", efsw::Actions::Add, file.Info );

	runPass( batch, watches );
	EXPECT_EQ( 0u, secondListener.getEventCount() );
	EXPECT_EQ( 0u, listener.getEventCount() );

	runPass( batch, watches );
	EXPECT_EQ( 3u, secondListener.getEventCount() );
	EXPECT_EQ( 0, secondListener.indexOf( efsw::Actions::Delete, "unrelated.txt" ) );
	EXPECT_EQ( 1, secondListener.indexOf( efsw::Actions::Add, "newparent" ) );
	EXPECT_EQ( 2, secondListener.indexOf( efsw::Actions::Moved, "file.txt" ) );
	EXPECT_EQ( 0u, listener.getEventCount() );
}

// A listener removes another registration while held events are delivered: the removed
// registration's events, already released into the batch, are not delivered.
UTEST( FileActionBatch, RemovingAnotherWatchCancelsReleasedEvents ) {
	START_BATCH_TEST();
	RemovingListener secondListener;
	TestListener thirdListener;
	second.Listener = &secondListener;
	third.Listener = &thirdListener;
	secondListener.Batch = &batch;
	secondListener.Removed = &third;
	second.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );
	third.report( "/other/", "unrelated.txt", efsw::Actions::Delete, file.make( "unrelated.txt" ) );

	runPass( batch, watches );
	EXPECT_EQ( 0u, secondListener.getEventCount() );

	runPass( batch, { &first, &second } );
	EXPECT_EQ( 1u, secondListener.getEventCount() );
	EXPECT_EQ( 0u, thirdListener.getEventCount() );
}

// The same, for events collected in the pass being delivered.
UTEST( FileActionBatch, RemovingAnotherWatchCancelsCollectedEvents ) {
	START_BATCH_TEST();
	RemovingListener firstListener;
	TestListener thirdListener;
	first.Listener = &firstListener;
	third.Listener = &thirdListener;
	firstListener.Batch = &batch;
	firstListener.Removed = &third;
	first.report( "/a/", "a.txt", efsw::Actions::Modified, efsw::FileInfo() );
	third.report( "/c/", "c.txt", efsw::Actions::Modified, efsw::FileInfo() );

	runPass( batch, watches );
	EXPECT_EQ( 1u, firstListener.getEventCount() );
	EXPECT_EQ( 0u, thirdListener.getEventCount() );
}

// A listener removes its own registration: its later events are not delivered.
UTEST( FileActionBatch, RemovingOwnWatchCancelsLaterEvents ) {
	START_BATCH_TEST();
	RemovingListener secondListener;
	second.Listener = &secondListener;
	secondListener.Batch = &batch;
	secondListener.Removed = &second;
	second.report( "/b/", "one.txt", efsw::Actions::Modified, efsw::FileInfo() );
	second.report( "/b/", "two.txt", efsw::Actions::Modified, efsw::FileInfo() );
	third.report( "/c/", "c.txt", efsw::Actions::Modified, efsw::FileInfo() );

	runPass( batch, watches );
	EXPECT_EQ( 1u, secondListener.getEventCount() );
	EXPECT_EQ( 1u, listener.getEventCount() );
}

// A watch removed while its own scan is still collecting: the events it already collected are
// canceled, though its scan has not ended yet.
UTEST( FileActionBatch, ForgetCancelsEventsOfAScanInProgress ) {
	START_BATCH_TEST();
	first.report( "/a/", "a.txt", efsw::Actions::Modified, efsw::FileInfo() );
	second.report( "/b/", "b.txt", efsw::Actions::Modified, efsw::FileInfo() );

	batch.beginPass();
	first.watch();
	batch.forget( &first );
	batch.scanned( &first );
	second.watch();
	batch.scanned( &second );
	third.watch();
	batch.scanned( &third );
	batch.finishPass();

	EXPECT_EQ( 1u, listener.getEventCount() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Modified, "b.txt" ) );
}

namespace {

/// Runs a pass where a listener removes the destination of a complete move during delivery.
/// @param removerFirst Whether the remover's event comes before the move's Delete.
void runCanceledDestination( bool removerFirst, MovedFile& file, int* utest_result ) {
	efsw::FileActionBatch batch;
	TestListener sourceListener;
	TestListener destinationListener;
	RemovingListener removerListener;
	FakeWatch source( 1, batch, &sourceListener );
	FakeWatch remover( 2, batch, &removerListener );
	FakeWatch destination( 3, batch, &destinationListener );
	removerListener.Batch = &batch;
	removerListener.Removed = &destination;

	efsw::FileInfo directory = file.make( "dir", true );
	efsw::FileInfo child = file.make( "dir/child.txt" );
	source.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );
	source.report( "/src/", "dir", efsw::Actions::Delete, directory );
	source.report( "/src/dir/", "child.txt", efsw::Actions::Delete, child );
	remover.report( "/other/", "other.txt", efsw::Actions::Modified, efsw::FileInfo() );
	destination.report( "/dst/", "file.txt", efsw::Actions::Add, file.Info );
	destination.report( "/dst/", "dir", efsw::Actions::Add, directory );

	if ( removerFirst )
		runPass( batch, { &remover, &source, &destination } );
	else
		runPass( batch, { &source, &remover, &destination } );

	// The source still gets its Deletes, including the one inside the moved directory; the
	// removed destination gets nothing.
	EXPECT_EQ( 3u, sourceListener.getEventCount() );
	EXPECT_NE( -1, sourceListener.indexOf( efsw::Actions::Delete, "file.txt" ) );
	EXPECT_NE( -1, sourceListener.indexOf( efsw::Actions::Delete, "dir" ) );
	EXPECT_NE( -1, sourceListener.indexOf( efsw::Actions::Delete, "child.txt" ) );
	EXPECT_EQ( 0u, destinationListener.getEventCount() );
}

} // namespace

// A listener removes the destination of a move that the batch already matched: the move
// falls apart, and the source, still registered, gets its Deletes. Both orders of the remover
// and the source in the batch.
UTEST( FileActionBatch, CanceledDestinationReleasesTheSourceDeletes ) {
	START_BATCH_TEST();
	(void)watches;
	runCanceledDestination( true, file, utest_result );
	runCanceledDestination( false, file, utest_result );
}

// A listener removes the source of a matched move: the move happened while both were
// registered, so the destination still gets its Moved, and the source nothing more.
UTEST( FileActionBatch, CanceledSourceStillDeliversTheMove ) {
	START_BATCH_TEST();
	TestListener sourceListener;
	TestListener destinationListener;
	RemovingListener removerListener;
	first.Listener = &removerListener;
	second.Listener = &sourceListener;
	third.Listener = &destinationListener;
	removerListener.Batch = &batch;
	removerListener.Removed = &second;
	first.report( "/other/", "other.txt", efsw::Actions::Modified, efsw::FileInfo() );
	second.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );
	third.report( "/dst/", "file.txt", efsw::Actions::Add, file.Info );

	runPass( batch, watches );

	EXPECT_EQ( 0u, sourceListener.getEventCount() );
	EXPECT_EQ( 1u, destinationListener.getEventCount() );
	EXPECT_EQ( 0, destinationListener.indexOf( efsw::Actions::Moved, "file.txt" ) );
}

namespace {

/// A source watch deletes file.txt, moved to the destination watch, then gets a new file at
/// the same path, with another identity. A listener removes the destination during delivery.
void runSourcePathReuse( bool heldDelete, MovedFile& file, int* utest_result ) {
	efsw::FileActionBatch batch;
	TestListener sourceListener;
	TestListener destinationListener;
	RemovingListener removerListener;
	FakeWatch remover( 1, batch, &removerListener );
	FakeWatch source( 2, batch, &sourceListener );
	FakeWatch destination( 3, batch, &destinationListener );
	removerListener.Batch = &batch;
	removerListener.Removed = &destination;
	efsw::FileInfo replacement = file.make( heldDelete ? "replacement1.txt" : "replacement2.txt" );

	source.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );
	if ( heldDelete ) {
		// The Delete waits for the next pass, where everything else happens.
		runPass( batch, { &remover, &source, &destination } );
		EXPECT_EQ( 0u, sourceListener.getEventCount() );
	}
	remover.report( "/other/", "other.txt", efsw::Actions::Modified, efsw::FileInfo() );
	source.report( "/src/", "file.txt", efsw::Actions::Add, replacement );
	destination.report( "/dst/", "file.txt", efsw::Actions::Add, file.Info );
	if ( heldDelete )
		runPass( batch, { &remover, &source, &destination } );
	else
		runPass( batch, { &source, &remover, &destination } );

	// The source sees the original file go, then the new one come.
	auto events = sourceListener.getEvents();
	ASSERT_EQ( 2u, events.size() );
	EXPECT_EQ( efsw::Actions::Delete, std::get<0>( events[0] ) );
	EXPECT_EQ( efsw::Actions::Add, std::get<0>( events[1] ) );
	EXPECT_EQ( 0u, destinationListener.getEventCount() );
}

} // namespace

// The move falls apart after its source Delete was skipped: the Delete is delivered before the
// later events of its watch, so a new file at the same path is not reported as deleted. With
// the Delete held from the previous pass, and within one pass.
UTEST( FileActionBatch, BrokenMoveKeepsTheSourceOrder ) {
	START_BATCH_TEST();
	(void)watches;
	runSourcePathReuse( true, file, utest_result );
	runSourcePathReuse( false, file, utest_result );
}

// The same with the move intact: the destination gets the Moved, and the source the new file,
// once the move is resolved.
UTEST( FileActionBatch, IntactMoveReleasesTheLaterSourceEvents ) {
	START_BATCH_TEST();
	TestListener sourceListener;
	TestListener destinationListener;
	first.Listener = &sourceListener;
	third.Listener = &destinationListener;
	efsw::FileInfo replacement = file.make( "replacement.txt" );
	first.report( "/src/", "file.txt", efsw::Actions::Delete, file.Info );
	first.report( "/src/", "file.txt", efsw::Actions::Add, replacement );
	second.report( "/other/", "other.txt", efsw::Actions::Modified, efsw::FileInfo() );
	third.report( "/dst/", "file.txt", efsw::Actions::Add, file.Info );

	runPass( batch, watches );

	auto events = sourceListener.getEvents();
	ASSERT_EQ( 1u, events.size() );
	EXPECT_EQ( efsw::Actions::Add, std::get<0>( events[0] ) );
	EXPECT_EQ( 1u, destinationListener.getEventCount() );
	EXPECT_EQ( 0, destinationListener.indexOf( efsw::Actions::Moved, "file.txt" ) );
	EXPECT_EQ( 1u, listener.getEventCount() );
}

namespace {

/// Crossed moves between two watches in one pass: one file from A to B, another from B to A.
/// Each watch's events wait for the other's move, so the moves are delivered as ordinary
/// events. @param cancel Whether A's listener removes B on its first event.
/// @param reuse Whether A gets a new file at the path the first file left.
/// @param directory Whether the first moved entry is a directory with a file inside.
void runCrossedMoves( bool cancel, bool reuse, bool directory, MovedFile& file,
					  int* utest_result ) {
	efsw::FileActionBatch batch;
	RemovingListener aListener;
	TestListener bListener;
	FakeWatch a( 1, batch, &aListener );
	FakeWatch b( 2, batch, &bListener );
	if ( cancel ) {
		aListener.Batch = &batch;
		aListener.Removed = &b;
	}
	const std::string tag =
		std::to_string( cancel ) + std::to_string( reuse ) + std::to_string( directory );
	efsw::FileInfo first = directory ? file.make( "dir" + tag, true ) : file.Info;
	efsw::FileInfo child = file.make( directory ? "dir" + tag + "/child.txt" : "child" + tag );
	efsw::FileInfo second = file.make( "second" + tag + ".txt" );
	efsw::FileInfo replacement = file.make( "replacement" + tag + ".txt" );

	a.report( "/A/", "a", efsw::Actions::Delete, first );
	if ( directory )
		a.report( "/A/a/", "child.txt", efsw::Actions::Delete, child );
	if ( reuse )
		a.report( "/A/", "a", efsw::Actions::Add, replacement );
	a.report( "/A/", "b", efsw::Actions::Add, second );
	b.report( "/B/", "b", efsw::Actions::Delete, second );
	b.report( "/B/", "a", efsw::Actions::Add, first );
	runPass( batch, { &a, &b } );

	// A gets every event of its own, in order: nothing is lost to the removal of B.
	std::vector<std::pair<efsw::Action, std::string>> expected;
	expected.push_back( { efsw::Actions::Delete, "a" } );
	if ( directory )
		expected.push_back( { efsw::Actions::Delete, "child.txt" } );
	if ( reuse )
		expected.push_back( { efsw::Actions::Add, "a" } );
	expected.push_back( { efsw::Actions::Add, "b" } );
	auto events = aListener.getEvents();
	ASSERT_EQ( expected.size(), events.size() );
	for ( size_t i = 0; i < expected.size(); i++ ) {
		EXPECT_EQ( expected[i].first, std::get<0>( events[i] ) );
		EXPECT_TRUE( expected[i].second == std::get<1>( events[i] ) );
	}

	if ( cancel ) {
		EXPECT_EQ( 0u, bListener.getEventCount() );
	} else {
		auto bEvents = bListener.getEvents();
		ASSERT_EQ( 2u, bEvents.size() );
		EXPECT_EQ( efsw::Actions::Delete, std::get<0>( bEvents[0] ) );
		EXPECT_EQ( efsw::Actions::Add, std::get<0>( bEvents[1] ) );
	}
}

} // namespace

UTEST( FileActionBatch, CrossedMovesAreDeliveredAsOrdinaryEvents ) {
	START_BATCH_TEST();
	(void)watches;
	runCrossedMoves( false, false, false, file, utest_result );
	runCrossedMoves( false, true, false, file, utest_result );
	runCrossedMoves( false, false, true, file, utest_result );
}

UTEST( FileActionBatch, CrossedMovesKeepSourceEventsWhenTheOtherWatchIsRemoved ) {
	START_BATCH_TEST();
	(void)watches;
	runCrossedMoves( true, false, false, file, utest_result );
	runCrossedMoves( true, true, false, file, utest_result );
	runCrossedMoves( true, false, true, file, utest_result );
}
