#include "test_util.hpp"
#include "utest.h"
#include <algorithm>
#include <array>
#include <efsw/FileActionBatch.hpp>
#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>
#include <map>
#include <set>

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

	bool snapshotHasAny( const std::vector<efsw::FileIdentity>& identities ) const override {
		for ( const efsw::FileIdentity& identity : Snapshot )
			if ( std::binary_search( identities.begin(), identities.end(), identity ) )
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
		Next.push_back( { directory, filename, action, std::move( info ) } );
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

// Entries whose names start with a moved directory's name, as dir2, dir-x and dir.txt start with
// dir, are not inside it: their events are their own. '-' and '.' sort before the separator.
UTEST( FileActionBatch, PrefixSiblingsStayOutOfAMovedDirectory ) {
	START_BATCH_TEST();
	efsw::FileInfo directory = file.make( "dir", true );
	efsw::FileInfo child = file.make( "dir/child.txt" );
	file.make( "dir2", true );
	file.make( "dir-x", true );
	efsw::FileInfo gone2 = file.make( "dir2/gone.txt" );
	efsw::FileInfo goneX = file.make( "dir-x/gone.txt" );
	efsw::FileInfo goneTxt = file.make( "dir.txt" );
	efsw::FileInfo new2 = file.make( "dir2/new.txt" );
	efsw::FileInfo newX = file.make( "dir-x/new.txt" );
	efsw::FileInfo newTxt = file.make( "new.txt" );
	first.report( "/src/", "dir", efsw::Actions::Delete, directory );
	first.report( "/src/dir/", "child.txt", efsw::Actions::Delete, child );
	first.report( "/src/dir2/", "gone2.txt", efsw::Actions::Delete, gone2 );
	first.report( "/src/dir-x/", "goneX.txt", efsw::Actions::Delete, goneX );
	first.report( "/src/", "dir.txt", efsw::Actions::Delete, goneTxt );
	second.report( "/dst/", "dir", efsw::Actions::Add, directory );
	second.report( "/dst/dir/", "child.txt", efsw::Actions::Add, child );
	second.report( "/dst/dir2/", "new2.txt", efsw::Actions::Add, new2 );
	second.report( "/dst/dir-x/", "newX.txt", efsw::Actions::Add, newX );
	second.report( "/dst/", "dir.txt", efsw::Actions::Add, newTxt );

	runPass( batch, watches );
	EXPECT_EQ( 7u, listener.getEventCount() );
	EXPECT_NE( -1, listener.indexOf( efsw::Actions::Moved, "dir" ) );
	EXPECT_EQ( -1, listener.indexOf( efsw::Actions::Delete, "child.txt" ) );
	EXPECT_EQ( -1, listener.indexOf( efsw::Actions::Add, "child.txt" ) );
	EXPECT_NE( -1, listener.indexOf( efsw::Actions::Delete, "gone2.txt" ) );
	EXPECT_NE( -1, listener.indexOf( efsw::Actions::Delete, "goneX.txt" ) );
	EXPECT_NE( -1, listener.indexOf( efsw::Actions::Delete, "dir.txt" ) );
	EXPECT_NE( -1, listener.indexOf( efsw::Actions::Add, "new2.txt" ) );
	EXPECT_NE( -1, listener.indexOf( efsw::Actions::Add, "newX.txt" ) );
	EXPECT_NE( -1, listener.indexOf( efsw::Actions::Add, "dir.txt" ) );
}

// A directory moved with a directory inside it, and a file inside that: the whole tree is one
// move, reported by a single Moved event, whatever order the events were collected in.
UTEST( FileActionBatch, NestedDirectoryMoveIsOneMovedEvent ) {
	START_BATCH_TEST();
	efsw::FileInfo outer = file.make( "outer", true );
	efsw::FileInfo inner = file.make( "outer/inner", true );
	efsw::FileInfo leaf = file.make( "outer/inner/leaf.txt" );
	for ( int deepestFirst = 0; deepestFirst < 2; deepestFirst++ ) {
		listener.clearEvents();
		if ( deepestFirst ) {
			first.report( "/src/outer/inner/", "leaf.txt", efsw::Actions::Delete, leaf );
			first.report( "/src/outer/", "inner", efsw::Actions::Delete, inner );
			first.report( "/src/", "outer", efsw::Actions::Delete, outer );
			second.report( "/dst/outer/inner/", "leaf.txt", efsw::Actions::Add, leaf );
			second.report( "/dst/outer/", "inner", efsw::Actions::Add, inner );
			second.report( "/dst/", "outer", efsw::Actions::Add, outer );
		} else {
			first.report( "/src/", "outer", efsw::Actions::Delete, outer );
			first.report( "/src/outer/", "inner", efsw::Actions::Delete, inner );
			first.report( "/src/outer/inner/", "leaf.txt", efsw::Actions::Delete, leaf );
			second.report( "/dst/", "outer", efsw::Actions::Add, outer );
			second.report( "/dst/outer/", "inner", efsw::Actions::Add, inner );
			second.report( "/dst/outer/inner/", "leaf.txt", efsw::Actions::Add, leaf );
		}
		runPass( batch, watches );
		auto events = listener.getEvents();
		EXPECT_EQ( 1u, events.size() );
		EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Moved, "outer" ) );
	}
}

namespace {

} // namespace

// A deeper scan can collect a Modified before the Add that completes its directory's move.
// Ordering those already observed events needs no snapshot evidence or tree reconstruction.
UTEST( FileActionBatch, ModifiedInsideDirectoryFollowsItsMove ) {
	if ( !efsw::FileInfo::inodeSupported() ) {
		UTEST_SKIP( "File identity is not supported." );
	}
	MovedFile file;
	ASSERT_TRUE( file.create() );
	const efsw::FileInfo directory = file.make( "outer", true );
	for ( int removal : { 0, 1, 2 } ) {
		efsw::FileActionBatch batch;
		TestListener sourceListener;
		RemovingListener destinationListener;
		FakeWatch source( 1, batch, &sourceListener );
		FakeWatch destination( 2, batch, &destinationListener );
		destinationListener.Batch = &batch;
		destinationListener.Removed = removal == 1 ? &source : removal == 2 ? &destination : NULL;
		source.report( "/src/", "outer", efsw::Actions::Delete, directory );
		destination.report( "/dst/outer/", "file.txt", efsw::Actions::Modified, file.Info );
		destination.report( "/dst/", "outer", efsw::Actions::Add, directory );
		runPass( batch, { &source, &destination } );

		EXPECT_EQ( 0u, sourceListener.getEventCount() );
		const auto events = destinationListener.getEvents();
		ASSERT_EQ( removal == 2 ? 1u : 2u, events.size() );
		EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
		EXPECT_TRUE( std::get<1>( events[0] ) == "outer" );
		if ( removal != 2 ) {
			EXPECT_EQ( efsw::Actions::Modified, std::get<0>( events[1] ) );
			EXPECT_TRUE( std::get<1>( events[1] ) == "file.txt" );
		}
	}
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

// A directory renamed within its directory while its contents changed, as when it left for
// another watch and came back under another name in one pass: its snapshot reports a Delete and
// an Add, which are the directory's move, by its name as a watch reports a rename. What moved
// with it keeps its path relative to it.
UTEST( FileActionBatch, DirectoryRenamedWhileItsContentsChanged ) {
	START_BATCH_TEST();
	efsw::FileInfo directory = file.make( "p", true );
	efsw::FileInfo inner = file.make( "p/x", true );
	efsw::FileInfo kept = file.make( "p/y" );
	first.report( "/a/", "r", efsw::Actions::Add, directory );
	first.report( "/a/r/", "x", efsw::Actions::Add, inner );
	first.report( "/a/", "p", efsw::Actions::Delete, directory );
	first.report( "/a/p/", "y", efsw::Actions::Delete, kept );
	first.report( "/a/p/", "x", efsw::Actions::Delete, inner );
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 1u, events.size() );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
	EXPECT_TRUE( std::get<1>( events[0] ) == "r" );
	EXPECT_TRUE( std::get<2>( events[0] ) == "p" );
}

// A directory added, then deleted with its parent, with both events in one pass: not a rename.
UTEST( FileActionBatch, AddAndDeleteAtOnePathAreNotARename ) {
	START_BATCH_TEST();
	efsw::FileInfo directory = file.make( "x", true );
	first.report( "/a/", "x", efsw::Actions::Add, directory );
	first.report( "/a/", "x", efsw::Actions::Delete, directory );
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 2u, events.size() );
	EXPECT_EQ( efsw::Actions::Add, std::get<0>( events[0] ) );
	EXPECT_EQ( efsw::Actions::Delete, std::get<0>( events[1] ) );
}

namespace {

} // namespace

namespace {

/// Records the full path of every event, to replay them.
class PathListener : public efsw::FileWatchListener {
  public:
	struct Recorded {
		efsw::Action Action;
		std::string Path;
		std::string OldPath;
	};
	std::vector<Recorded> Events;

	void handleFileAction( efsw::WatchID, const std::string& dir, const std::string& filename,
						   efsw::Action action, const std::string& oldFilename ) override {
		Events.push_back( { action, dir + filename, oldFilename } );
	}
};

const efsw::FileIdentity Unknown( 0, 0 );

/// A tree of paths, to which the delivered events are applied as a listener would apply them:
/// each must name an entry that exists, or a free name in a directory that exists.
class ReplayTree {
  public:
	std::map<std::string, efsw::FileIdentity> Entries;
	/// The paths deleted so far.
	std::vector<std::string> Deleted;

	/// @return An empty string, or what was wrong with the event.
	std::string apply( PathListener::Recorded event ) {
		// Moved events give their sources canonically, with a drive on Windows; the fake paths
		// have none.
		event.Path = withoutDrive( event.Path );
		event.OldPath = withoutDrive( event.OldPath );
		switch ( event.Action ) {
			case efsw::Actions::Delete:
				if ( !Entries.count( event.Path ) ) {
					// The generic backend reports the entries of a deleted directory after it.
					for ( const std::string& deleted : Deleted )
						if ( 0 == event.Path.compare( 0, deleted.size() + 1, deleted + slash() ) )
							return "";
					return "deletes a missing " + event.Path;
				}
				erase( event.Path );
				Deleted.push_back( event.Path );
				return "";
			case efsw::Actions::Add:
				if ( Entries.count( event.Path ) )
					return "adds an existing " + event.Path;
				if ( !Entries.count( parent( event.Path ) ) )
					return "adds into a missing parent: " + event.Path;
				Entries[event.Path] = Unknown;
				return "";
			case efsw::Actions::Moved: {
				if ( !Entries.count( event.OldPath ) )
					return "moves a missing " + event.OldPath;
				if ( Entries.count( event.Path ) )
					return "moves onto an existing " + event.Path;
				if ( !Entries.count( parent( event.Path ) ) )
					return "moves into a missing parent: " + event.Path;
				if ( 0 ==
					 event.Path.compare( 0, event.OldPath.size() + 1, event.OldPath + slash() ) )
					return "moves into itself: " + event.Path;
				std::map<std::string, efsw::FileIdentity> moved;
				moved[event.Path] = Entries[event.OldPath];
				const std::string below = event.OldPath + slash();
				for ( auto it = Entries.lower_bound( below );
					  it != Entries.end() && 0 == it->first.compare( 0, below.size(), below );
					  ++it )
					moved[event.Path + it->first.substr( event.OldPath.size() )] = it->second;
				erase( event.OldPath );
				Entries.insert( moved.begin(), moved.end() );
				return "";
			}
			default:
				return "";
		}
	}

	static std::string slash() { return std::string( 1, efsw::FileSystem::getOSSlash() ); }

	static std::string withoutDrive( const std::string& path ) {
		return path.size() >= 2 && path[1] == ':' ? path.substr( 2 ) : path;
	}

  private:
	static std::string parent( const std::string& path ) {
		return path.substr( 0, path.rfind( efsw::FileSystem::getOSSlash() ) );
	}

	void erase( const std::string& path ) {
		Entries.erase( path );
		const std::string below = path + slash();
		auto it = Entries.lower_bound( below );
		while ( it != Entries.end() && 0 == it->first.compare( 0, below.size(), below ) )
			it = Entries.erase( it );
	}
};

efsw::FileInfo synthetic( const std::string& path, const efsw::FileIdentity& identity,
						  bool directory ) {
	efsw::FileInfo info;
	info.Filepath = path;
	info.Device = identity.first;
	info.Inode = identity.second;
	info.Size = 1;
	info.ModificationTime = 1;
	info.LinkCount = 1;
	info.Permissions = directory ? 040755 : 0100644;
	return info;
}

/// Reports an event at a full path from the watch.
void reportAt( FakeWatch& watch, const std::string& path, efsw::Action action,
			   const std::pair<efsw::FileIdentity, bool>& entry ) {
	const size_t slash = path.rfind( efsw::FileSystem::getOSSlash() );
	watch.report( path.substr( 0, slash + 1 ), path.substr( slash + 1 ), action,
				  synthetic( path, entry.first, entry.second ) );
}

/// The identity the replay keeps for an entry: with its kind, so that an entry replaced by one of
/// another kind that reuses its identity, a directory where a file was, is another entry.
efsw::FileIdentity replayed( const std::pair<efsw::FileIdentity, bool>& entry ) {
	return entry.second
			   ? efsw::FileIdentity( entry.first.first | ( 1ull << 62 ), entry.first.second )
			   : entry.first;
}

std::string at( std::initializer_list<const char*> names ) {
	std::string path;
	for ( const char* name : names )
		path += ReplayTree::slash() + name;
	return path;
}

efsw::FileIdentity id( unsigned long inode ) {
	return efsw::FileIdentity( 1, inode );
}

enum class RemovalBeforeChildDelete { None, Source, Destination };

// Seed 370: a directory leaves all watches in one pass; its parent moves in the next while
// a file returns from the departed directory. Its released Delete waits for the file, but
// must still name a path the consumer knows when the parent is moved to another registration.
void checkReleasedChildDelete( bool queuedMove, RemovalBeforeChildDelete removal, int* utest_result,
							   const std::array<int, 3>& order = { { 0, 1, 2 } },
							   bool returnInside = false ) {
	efsw::FileActionBatch batch;
	struct Listener : PathListener {
		efsw::FileActionBatch* Batch;
		efsw::Watcher* Removed{ NULL };

		void handleFileAction( efsw::WatchID watch, const std::string& directory,
							   const std::string& filename, efsw::Action action,
							   const std::string& old ) override {
			PathListener::handleFileAction( watch, directory, filename, action, old );
			if ( Removed ) {
				Batch->forget( Removed );
				Removed = NULL;
			}
		}
	} listener;
	listener.Batch = &batch;
	FakeWatch destination( 1, batch, &listener );
	FakeWatch source( 2, batch, &listener );
	FakeWatch other( 3, batch, &listener );
	const std::vector<FakeWatch*> watches{ &destination, &source, &other };
	const std::string from = at( { "src", "outer" } );
	const std::string child = from + at( { "inner" } );
	const std::string to = at( { "dst", "outer" } );
	const std::string returned = returnInside ? to + at( { "file" } ) : at( { "other", "file" } );
	const std::string unrelated = at( { "src", "unrelated" } );
	const std::string relocated = at( { "other", "unrelated" } );
	const std::pair<efsw::FileIdentity, bool> outer{ id( 10 ), true };
	const std::pair<efsw::FileIdentity, bool> inner{ id( 11 ), true };
	const std::pair<efsw::FileIdentity, bool> leaf{ id( 12 ), false };
	const std::pair<efsw::FileIdentity, bool> crossing{ id( 13 ), false };
	ReplayTree replay;
	for ( const std::string& root : { at( { "src" } ), at( { "dst" } ), at( { "other" } ) } )
		replay.Entries[root] = Unknown;
	replay.Entries[from] = replayed( outer );
	replay.Entries[child] = replayed( inner );
	replay.Entries[child + at( { "file" } )] = replayed( leaf );
	if ( queuedMove ) {
		replay.Entries[unrelated] = replayed( crossing );
		reportAt( source, unrelated, efsw::Actions::Delete, crossing );
	}
	reportAt( source, child, efsw::Actions::Delete, inner );
	reportAt( source, child + at( { "file" } ), efsw::Actions::Delete, leaf );
	runPass( batch, watches );
	ASSERT_EQ( 0u, listener.Events.size() );

	listener.Removed = removal == RemovalBeforeChildDelete::Source		  ? &source
					   : removal == RemovalBeforeChildDelete::Destination ? &destination
																		  : NULL;
	reportAt( destination, to, efsw::Actions::Add, outer );
	reportAt( source, from, efsw::Actions::Delete, outer );
	reportAt( returnInside ? destination : other, returned, efsw::Actions::Add, leaf );
	if ( queuedMove )
		reportAt( other, relocated, efsw::Actions::Add, crossing );
	const std::vector<FakeWatch*> scans{ watches[order[0]], watches[order[1]], watches[order[2]] };
	runPass( batch, scans );
	runPass( batch, scans );
	if ( removal == RemovalBeforeChildDelete::Source ) {
		// Removing the source cancels its pending Deletes, but not moves into other watches.
		ASSERT_EQ( 2u, listener.Events.size() );
		EXPECT_EQ( efsw::Actions::Moved, listener.Events[0].Action );
		EXPECT_EQ( efsw::Actions::Moved, listener.Events[1].Action );
		EXPECT_STREQ( returned.c_str(), listener.Events[0].Path.c_str() );
		EXPECT_STREQ( to.c_str(), listener.Events[1].Path.c_str() );
		return;
	}
	std::string problem;
	for ( const auto& event : listener.Events ) {
		problem = replay.apply( event );
		if ( !problem.empty() )
			break;
	}
	if ( !problem.empty() ) {
		for ( const auto& event : listener.Events )
			printf( "  %d %s <- %s\n", (int)event.Action, event.Path.c_str(),
					event.OldPath.c_str() );
	}
	EXPECT_STREQ( "", problem.c_str() );
	const bool destinationRemoved = removal == RemovalBeforeChildDelete::Destination;
	EXPECT_EQ( 5u + queuedMove - destinationRemoved, replay.Entries.size() );
	for ( const std::string& path :
		  { at( { "src" } ), at( { "dst" } ), at( { "other" } ), returned } )
		EXPECT_EQ( 1u, replay.Entries.count( path ) );
	EXPECT_EQ( destinationRemoved ? 0u : 1u, replay.Entries.count( to ) );
	if ( queuedMove )
		EXPECT_EQ( 1u, replay.Entries.count( relocated ) );
}

} // namespace

UTEST( FileActionBatch, ReleasedChildDeleteRemainsValidWhenItsParentMoves ) {
	checkReleasedChildDelete( false, RemovalBeforeChildDelete::None, utest_result );
}

UTEST( FileActionBatch, ReleasedChildDeleteBehindAnotherMoveRemainsValid ) {
	checkReleasedChildDelete( true, RemovalBeforeChildDelete::None, utest_result );
}

UTEST( FileActionBatch, RemovingSourceWhileItsChildDeleteBlocksItsMove ) {
	checkReleasedChildDelete( false, RemovalBeforeChildDelete::Source, utest_result );
}

UTEST( FileActionBatch, RemovingDestinationWhileAChildDeleteBlocksItsMove ) {
	checkReleasedChildDelete( false, RemovalBeforeChildDelete::Destination, utest_result );
}

UTEST( FileActionBatch, ReleasedChildDeleteDependenciesInEveryScanOrder ) {
	std::array<int, 3> order{ { 0, 1, 2 } };
	do {
		checkReleasedChildDelete( false, RemovalBeforeChildDelete::None, utest_result, order );
		checkReleasedChildDelete( true, RemovalBeforeChildDelete::None, utest_result, order );
		checkReleasedChildDelete( false, RemovalBeforeChildDelete::Source, utest_result, order );
		checkReleasedChildDelete( false, RemovalBeforeChildDelete::Destination, utest_result,
								  order );
		// Returning into the parent's destination creates a dependency cycle. A conservative
		// Delete/Add fallback must still replay correctly, rather than delivering stale paths.
		checkReleasedChildDelete( false, RemovalBeforeChildDelete::None, utest_result, order,
								  true );
	} while ( std::next_permutation( order.begin(), order.end() ) );
}

// The dependency index searches virtual "path + separator" bounds in place. A neighboring
// name must not become a child of the moved directory, including bytes above ASCII: doing so
// makes its Delete block the move while its file's arrival waits behind that move, a cycle.
UTEST( FileActionBatch, PrefixSiblingDeletesDoNotBlockADirectoryMove ) {
	for ( const char* suffix : { "-x", "2", ".txt", "\xc3\xa9" } ) {
		efsw::FileActionBatch batch;
		TestListener listener;
		FakeWatch destination( 1, batch, &listener );
		FakeWatch source( 2, batch, &listener );
		FakeWatch other( 3, batch, &listener );
		const std::vector<FakeWatch*> watches{ &destination, &source, &other };
		const std::string from = at( { "src", "outer" } );
		const std::string sibling = from + suffix + at( { "inner" } );
		const std::string to = at( { "dst", "outer" } );
		const std::pair<efsw::FileIdentity, bool> outer{ id( 10 ), true };
		const std::pair<efsw::FileIdentity, bool> inner{ id( 11 ), true };
		const std::pair<efsw::FileIdentity, bool> leaf{ id( 12 ), false };
		reportAt( source, sibling, efsw::Actions::Delete, inner );
		reportAt( source, sibling + at( { "file" } ), efsw::Actions::Delete, leaf );
		runPass( batch, watches );
		ASSERT_EQ( 0u, listener.getEventCount() );
		reportAt( destination, to, efsw::Actions::Add, outer );
		reportAt( destination, to + at( { "file" } ), efsw::Actions::Add, leaf );
		reportAt( source, from, efsw::Actions::Delete, outer );
		runPass( batch, watches );
		runPass( batch, watches );
		EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Moved, "outer" ) );
		EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Moved, "file" ) );
		EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Delete, "inner" ) );
	}
}

namespace {

} // namespace

namespace {

} // namespace

// Directories may be reported with a final separator, as the generic backend reports them: a
// moved directory does not enclose itself, and a change in its new tree, collected before it,
// follows it.
UTEST( FileActionBatch, DirectoryPathsWithAFinalSeparator ) {
	const std::string slash = ReplayTree::slash();
	efsw::FileActionBatch batch;
	TestListener listener;
	FakeWatch source( 1, batch, &listener );
	FakeWatch destination( 2, batch, &listener );
	const efsw::FileIdentity directory = id( 10 ), file = id( 11 );
	auto withSeparator = [&]( FakeWatch& watch, const std::string& parent, const std::string& name,
							  efsw::Action action ) {
		watch.report( parent, name, action, synthetic( "", directory, true ) );
		watch.Next.back().Info.Filepath = parent + name + slash;
	};
	reportAt( destination, slash + "dst" + slash + "d" + slash + "changed", efsw::Actions::Modified,
			  { file, false } );
	withSeparator( source, slash + "src" + slash, "d", efsw::Actions::Delete );
	withSeparator( destination, slash + "dst" + slash, "d", efsw::Actions::Add );
	runPass( batch, { &source, &destination } );

	auto events = listener.getEvents();
	ASSERT_EQ( 2u, events.size() );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
	EXPECT_TRUE( std::get<1>( events[0] ) == "d" );
	EXPECT_EQ( efsw::Actions::Modified, std::get<0>( events[1] ) );
	EXPECT_TRUE( std::get<1>( events[1] ) == "changed" );
}

namespace {

/// Two moves that wait on each other in a chain: one goes from the first watch to the second,
/// and two goes from the second watch to the third. The second watch reports the Delete of two
/// before the Add of one, so the first watch's events wait for the second watch, which waits
/// for the third. Resolving each move wakes the next registration in the chain, whose events
/// then come before what the third watch reports later.
void reportChainedMoves( FakeWatch& first, FakeWatch& second, FakeWatch& third ) {
	first.report( "/a/", "one", efsw::Actions::Delete, synthetic( "", id( 10 ), false ) );
	first.report( "/a/", "first", efsw::Actions::Modified, synthetic( "", id( 30 ), false ) );
	second.report( "/b/", "two", efsw::Actions::Delete, synthetic( "", id( 11 ), false ) );
	second.report( "/b/", "one", efsw::Actions::Add, synthetic( "", id( 10 ), false ) );
	third.report( "/c/", "two", efsw::Actions::Add, synthetic( "", id( 11 ), false ) );
	third.report( "/c/", "third", efsw::Actions::Modified, synthetic( "", id( 31 ), false ) );
}

/// A directory removed from the first watch after both of its files were moved out of it, one
/// to the second watch and one to the third. Its Delete waits on both moves, and comes right
/// after the second of them, before what the watch of that move reports after it.
void reportDeleteAfterTwoMoves( FakeWatch& first, FakeWatch& second, FakeWatch& third ) {
	const std::string slash = ReplayTree::slash();
	const std::string gone = slash + "a" + slash + "gone" + slash;
	first.report( slash + "a" + slash, "gone", efsw::Actions::Delete,
				  synthetic( "", id( 20 ), true ) );
	first.report( gone, "one", efsw::Actions::Delete, synthetic( "", id( 10 ), false ) );
	first.report( gone, "two", efsw::Actions::Delete, synthetic( "", id( 11 ), false ) );
	second.report( slash + "b" + slash, "one", efsw::Actions::Add,
				   synthetic( "", id( 10 ), false ) );
	second.report( slash + "b" + slash, "second", efsw::Actions::Modified,
				   synthetic( "", id( 31 ), false ) );
	third.report( slash + "c" + slash, "two", efsw::Actions::Add,
				  synthetic( "", id( 11 ), false ) );
	third.report( slash + "c" + slash, "third", efsw::Actions::Modified,
				  synthetic( "", id( 30 ), false ) );
}

void expectEvents( TestListener& listener,
				   const std::vector<std::pair<efsw::Action, std::string>>& expected,
				   int* utest_result ) {
	auto events = listener.getEvents();
	ASSERT_EQ( expected.size(), events.size() );
	for ( size_t i = 0; i < expected.size(); i++ ) {
		EXPECT_EQ( expected[i].first, std::get<0>( events[i] ) );
		EXPECT_TRUE( expected[i].second == std::get<1>( events[i] ) );
	}
}

} // namespace

UTEST( FileActionBatch, ChainedMovesWakeEachWaitingWatch ) {
	efsw::FileActionBatch batch;
	TestListener listener;
	FakeWatch first( 1, batch, &listener );
	FakeWatch second( 2, batch, &listener );
	FakeWatch third( 3, batch, &listener );
	reportChainedMoves( first, second, third );
	runPass( batch, { &first, &second, &third } );

	expectEvents( listener,
				  { { efsw::Actions::Moved, "two" },
					{ efsw::Actions::Moved, "one" },
					{ efsw::Actions::Modified, "first" },
					{ efsw::Actions::Modified, "third" } },
				  utest_result );
}

// The same chain, but the third watch's listener removes the second registration when it
// receives its move: the move into the second watch is canceled, so the first watch, woken
// by the cancellation, gets its file's Delete, before what the third watch reports later.
UTEST( FileActionBatch, RemovingAWatchWhileWakingTheChain ) {
	efsw::FileActionBatch batch;
	RemovingListener listener;
	FakeWatch first( 1, batch, &listener );
	FakeWatch second( 2, batch, &listener );
	FakeWatch third( 3, batch, &listener );
	listener.Batch = &batch;
	listener.Removed = &second;
	reportChainedMoves( first, second, third );
	runPass( batch, { &first, &second, &third } );

	expectEvents( listener,
				  { { efsw::Actions::Moved, "two" },
					{ efsw::Actions::Delete, "one" },
					{ efsw::Actions::Modified, "first" },
					{ efsw::Actions::Modified, "third" } },
				  utest_result );
}

UTEST( FileActionBatch, DeleteWaitsForTwoMovesInScanOrder ) {
	efsw::FileActionBatch batch;
	TestListener listener;
	FakeWatch first( 1, batch, &listener );
	FakeWatch second( 2, batch, &listener );
	FakeWatch third( 3, batch, &listener );
	reportDeleteAfterTwoMoves( first, second, third );
	runPass( batch, { &first, &second, &third } );

	expectEvents( listener,
				  { { efsw::Actions::Moved, "one" },
					{ efsw::Actions::Modified, "second" },
					{ efsw::Actions::Moved, "two" },
					{ efsw::Actions::Delete, "gone" },
					{ efsw::Actions::Modified, "third" } },
				  utest_result );
}

// The same, with the second move's destination scanned, and resolved, first.
UTEST( FileActionBatch, DeleteWaitsForTwoMovesInReverseOrder ) {
	efsw::FileActionBatch batch;
	TestListener listener;
	FakeWatch first( 1, batch, &listener );
	FakeWatch second( 2, batch, &listener );
	FakeWatch third( 3, batch, &listener );
	reportDeleteAfterTwoMoves( first, second, third );
	runPass( batch, { &first, &third, &second } );

	expectEvents( listener,
				  { { efsw::Actions::Moved, "two" },
					{ efsw::Actions::Modified, "third" },
					{ efsw::Actions::Moved, "one" },
					{ efsw::Actions::Delete, "gone" },
					{ efsw::Actions::Modified, "second" } },
				  utest_result );
}

// A file moved into a subdirectory, and another one added at its old path, all in one watch's
// pass: delivered as a move at its destination, it would follow the new file's Add, and name
// the new file's path as its source. Its halves are reported instead, in their order.
UTEST( FileActionBatch, MoveWhoseSourcePathIsTakenFirstIsNotPaired ) {
	START_BATCH_TEST();
	efsw::FileInfo moved = file.make( "moved" );
	efsw::FileInfo added = file.make( "added" );
	second.report( "/w/", "g", efsw::Actions::Delete, moved );
	second.report( "/w/", "g", efsw::Actions::Add, added );
	second.report( "/w/d/", "g", efsw::Actions::Add, moved );
	runPass( batch, watches );

	expectEvents( listener,
				  { { efsw::Actions::Delete, "g" },
					{ efsw::Actions::Add, "g" },
					{ efsw::Actions::Add, "g" } },
				  utest_result );
}
