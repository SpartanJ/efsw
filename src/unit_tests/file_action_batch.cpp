#include "test_util.hpp"
#include "utest.h"
#include <algorithm>
#include <array>
#include <efsw/FileActionBatch.hpp>
#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>
#include <map>
#include <random>
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
		/// The old name of a Moved event, in its directory.
		std::string Old;
		/// The ID reported, when not the watch's: kqueue reports each directory with its own.
		efsw::WatchID Id;
	};

	efsw::FileActionBatch& Batch;
	/// Identities listed in the watch's snapshot.
	std::set<efsw::FileIdentity> Snapshot;
	/// Events reported by the next scan.
	std::vector<Pending> Next;
	/// Evidence of directory trees that left the watch, given up by the next scan.
	std::vector<std::pair<efsw::FileInfo, std::vector<efsw::FileInfoList>>> GiveUp;
	/// The entries of the trees the watch watches, for collectTree().
	std::vector<efsw::FileInfo> Tree;
	/// Whether the watch watches inside the directories of Tree.
	bool WatchesTrees{ true };
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
			Batch.add( event.Id ? event.Id : ID, event.Directory, event.Filename, event.Action,
					   event.Old, event.Info, Listener, this );
		Next.clear();
		for ( auto& tree : GiveUp )
			Batch.addSourceTree( tree.first, std::move( tree.second ) );
		GiveUp.clear();
	}

	bool collectTree( const std::string& directory,
					  std::vector<const efsw::FileInfo*>* entries ) override {
		if ( !WatchesTrees )
			return false;
		const std::string prefix = directory + efsw::FileSystem::getOSSlash();
		for ( const efsw::FileInfo& entry : Tree )
			if ( entries && 0 == entry.Filepath.compare( 0, prefix.size(), prefix ) )
				entries->push_back( &entry );
		return true;
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

/// The entry with its path set, in the platform's separators, as snapshots list them.
efsw::FileInfo at( efsw::FileInfo info, const std::string& path ) {
	info.Filepath = path;
	for ( char& c : info.Filepath )
		if ( c == '/' )
			c = efsw::FileSystem::getOSSlash();
	return info;
}

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

// Entries renamed or relocated inside a directory moved as a whole are moves of their own: the
// directory's Moved event does not report their new names. A renamed directory's contents keep
// their path relative to it, and are reported by its Moved event. An entry moved into the tree
// from elsewhere is a move of its own too.
UTEST( FileActionBatch, RenamesInsideAMovedTreeAreMovesOfTheirOwn ) {
	START_BATCH_TEST();
	efsw::FileInfo outer = file.make( "outer", true );
	efsw::FileInfo inner = file.make( "outer/inner", true );
	efsw::FileInfo leaf = file.make( "outer/inner/leaf.txt" );
	efsw::FileInfo sub = file.make( "outer/sub", true );
	efsw::FileInfo relocated = file.make( "outer/relocated.txt" );
	efsw::FileInfo renamed = file.make( "outer/renamed.txt" );
	efsw::FileInfo kept = file.make( "outer/kept.txt" );
	efsw::FileInfo elsewhere = file.make( "elsewhere.txt" );
	for ( int deepestFirst = 0; deepestFirst < 2; deepestFirst++ ) {
		listener.clearEvents();
		std::vector<std::pair<std::string, std::string>> sources{
			{ "/src/", "outer" },
			{ "/src/outer/", "inner" },
			{ "/src/outer/inner/", "leaf.txt" },
			{ "/src/outer/", "sub" },
			{ "/src/outer/", "relocated.txt" },
			{ "/src/outer/", "renamed.txt" },
			{ "/src/outer/", "kept.txt" },
			{ "/src/", "elsewhere.txt" } };
		std::vector<std::pair<std::string, std::string>> destinations{
			{ "/dst/", "outer" },
			{ "/dst/outer/", "inner2" },
			{ "/dst/outer/inner2/", "leaf.txt" },
			{ "/dst/outer/", "sub" },
			{ "/dst/outer/sub/", "relocated.txt" },
			{ "/dst/outer/", "renamed2.txt" },
			{ "/dst/outer/", "kept.txt" },
			{ "/dst/outer/", "elsewhere.txt" } };
		std::vector<efsw::FileInfo> infos{ outer,	  inner,   leaf, sub,
										   relocated, renamed, kept, elsewhere };
		for ( size_t n = 0; n < infos.size(); n++ ) {
			size_t i = deepestFirst ? infos.size() - 1 - n : n;
			first.report( sources[i].first, sources[i].second, efsw::Actions::Delete, infos[i] );
		}
		for ( size_t n = 0; n < infos.size(); n++ ) {
			size_t i = deepestFirst ? infos.size() - 1 - n : n;
			second.report( destinations[i].first, destinations[i].second, efsw::Actions::Add,
						   infos[i] );
		}
		runPass( batch, watches );

		auto events = listener.getEvents();
		EXPECT_EQ( 5u, events.size() );
		for ( const auto& event : events )
			EXPECT_EQ( efsw::Actions::Moved, std::get<0>( event ) );
		// The enclosing move comes first: the others happened in its new tree.
		EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Moved, "outer" ) );
		EXPECT_NE( -1, listener.indexOf( efsw::Actions::Moved, "inner2" ) );
		EXPECT_NE( -1, listener.indexOf( efsw::Actions::Moved, "relocated.txt" ) );
		EXPECT_NE( -1, listener.indexOf( efsw::Actions::Moved, "renamed2.txt" ) );
		EXPECT_NE( -1, listener.indexOf( efsw::Actions::Moved, "elsewhere.txt" ) );
		// From where the directory's Moved event put it.
		std::string innerSource( "/dst/outer/inner" );
		std::replace( innerSource.begin(), innerSource.end(), '/', efsw::FileSystem::getOSSlash() );
		EXPECT_TRUE( listener.checkEvent( efsw::Actions::Moved, "inner2", innerSource ) );
	}
}

namespace {

/// A directory moved with a directory renamed inside it and a file modified there, collected
/// deepest first, as a backend that scans deeper directories first collects them.
/// @param removal 0: none. 1: the destination's listener removes the source registration on
/// its first event. 2: an earlier registration's listener removes the destination's.
void runNestedOrder( int removal, MovedFile& file, int* utest_result ) {
	efsw::FileActionBatch batch;
	TestListener sourceListener;
	RemovingListener destinationListener;
	RemovingListener earlierListener;
	FakeWatch earlier( 3, batch, &earlierListener );
	FakeWatch source( 1, batch, &sourceListener );
	FakeWatch destination( 2, batch, &destinationListener );
	if ( removal == 1 ) {
		destinationListener.Batch = &batch;
		destinationListener.Removed = &source;
	} else if ( removal == 2 ) {
		earlierListener.Batch = &batch;
		earlierListener.Removed = &destination;
	}
	const std::string tag = std::to_string( removal );
	efsw::FileInfo outer = file.make( "outer" + tag, true );
	efsw::FileInfo inner = file.make( "outer" + tag + "/inner", true );
	efsw::FileInfo leaf = file.make( "outer" + tag + "/inner/leaf.txt" );
	efsw::FileInfo unrelated = file.make( "unrelated" + tag + ".txt" );

	earlier.report( "/other/", "unrelated.txt", efsw::Actions::Add, unrelated );
	source.report( "/src/outer/inner/", "leaf.txt", efsw::Actions::Delete, leaf );
	source.report( "/src/outer/", "inner", efsw::Actions::Delete, inner );
	source.report( "/src/", "outer", efsw::Actions::Delete, outer );
	destination.report( "/dst/outer/inner2/", "leaf.txt", efsw::Actions::Add, leaf );
	destination.report( "/dst/outer/inner2/", "leaf.txt", efsw::Actions::Modified, leaf );
	destination.report( "/dst/outer/", "inner2", efsw::Actions::Add, inner );
	destination.report( "/dst/", "outer", efsw::Actions::Add, outer );
	runPass( batch, { &earlier, &source, &destination } );

	if ( removal == 2 ) {
		// The moves fell apart with their destination: the source gets its own events, in order.
		EXPECT_EQ( 0u, destinationListener.getEventCount() );
		auto events = sourceListener.getEvents();
		ASSERT_EQ( 3u, events.size() );
		EXPECT_TRUE( std::get<1>( events[0] ) == "leaf.txt" );
		EXPECT_TRUE( std::get<1>( events[1] ) == "inner" );
		EXPECT_TRUE( std::get<1>( events[2] ) == "outer" );
		for ( const auto& event : events )
			EXPECT_EQ( efsw::Actions::Delete, std::get<0>( event ) );
		return;
	}
	EXPECT_EQ( 0u, sourceListener.getEventCount() );
	auto events = destinationListener.getEvents();
	ASSERT_EQ( 3u, events.size() );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
	EXPECT_TRUE( std::get<1>( events[0] ) == "outer" );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[1] ) );
	EXPECT_TRUE( std::get<1>( events[1] ) == "inner2" );
	EXPECT_EQ( efsw::Actions::Modified, std::get<0>( events[2] ) );
	EXPECT_TRUE( std::get<1>( events[2] ) == "leaf.txt" );
}

} // namespace

// Events in a moved directory's new tree are delivered after its Moved event, and those in a
// directory moved inside it after that one's, however they were collected, also when a
// listener removes the source or the destination registration during delivery.
UTEST( FileActionBatch, NestedEventsFollowTheirEnclosingMove ) {
	START_BATCH_TEST();
	(void)watches;
	runNestedOrder( 0, file, utest_result );
	runNestedOrder( 1, file, utest_result );
	runNestedOrder( 2, file, utest_result );
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

namespace {

/// A directory tree moved from the first watch to the second, which has evidence of it on both
/// ends: the first watch gives up the snapshots of its tree, and the second watches the tree.
struct MovedTree {
	efsw::FileInfo Outer;
	std::vector<efsw::FileInfoList> Before;

	explicit MovedTree( MovedFile& file ) { Outer = file.make( "outer", true ); }

	/// An entry of the tree before the move, by its path below outer.
	void was( const efsw::FileInfo& info, const std::string& relative ) {
		if ( Before.empty() )
			Before.emplace_back();
		Before[0].push_back( at( info, "/src/outer/" + relative ) );
	}

	/// An entry of the tree after the move, as the destination's snapshots list it.
	void is( FakeWatch& destination, const efsw::FileInfo& info, const std::string& relative ) {
		destination.Tree.push_back( at( info, "/dst/outer/" + relative ) );
	}

	void move( FakeWatch& source, FakeWatch& destination ) {
		source.report( "/src/", "outer", efsw::Actions::Delete, Outer );
		source.GiveUp.emplace_back( at( Outer, "/src/outer" ), std::move( Before ) );
		destination.report( "/dst/", "outer", efsw::Actions::Add, Outer );
	}
};

std::string slashes( std::string path ) {
	for ( char& c : path )
		if ( c == '/' )
			c = efsw::FileSystem::getOSSlash();
	return path;
}

} // namespace

// A directory tree moved as a whole: one Moved event, and nothing for the entries it kept.
UTEST( FileActionBatch, UnchangedTreeIsOneMovedEvent ) {
	START_BATCH_TEST();
	MovedTree tree( file );
	efsw::FileInfo inner = file.make( "outer/inner", true );
	efsw::FileInfo leaf = file.make( "outer/inner/leaf.txt" );
	efsw::FileInfo top = file.make( "outer/top.txt" );
	tree.was( inner, "inner" );
	tree.was( leaf, "inner/leaf.txt" );
	tree.was( top, "top.txt" );
	tree.is( second, inner, "inner" );
	tree.is( second, leaf, "inner/leaf.txt" );
	tree.is( second, top, "top.txt" );
	tree.move( first, second );
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 1u, events.size() );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
	EXPECT_TRUE( std::get<1>( events[0] ) == "outer" );
}

// What changed inside a moved directory tree before it was scanned is reported after its Moved
// event: deleted entries first, each before its parent directory, where the Moved event put
// them; then the entries that arrived, parents first, each moved one from where the moves before
// it put it.
UTEST( FileActionBatch, ChangesInsideAMovedTreeFollowItsMovedEvent ) {
	START_BATCH_TEST();
	MovedTree tree( file );
	efsw::FileInfo inner = file.make( "outer/inner", true );
	efsw::FileInfo leaf = file.make( "outer/inner/leaf.txt" );
	efsw::FileInfo renamedFile = file.make( "outer/inner/x.txt" );
	efsw::FileInfo deletedInside = file.make( "outer/inner/del.txt" );
	efsw::FileInfo a = file.make( "outer/a.txt" );
	efsw::FileInfo c = file.make( "outer/c.txt" );
	efsw::FileInfo gone = file.make( "outer/gone.txt" );
	efsw::FileInfo kept = file.make( "outer/kept.txt" );
	efsw::FileInfo created = file.make( "outer/new.txt" );
	tree.was( inner, "inner" );
	tree.was( leaf, "inner/leaf.txt" );
	tree.was( renamedFile, "inner/x.txt" );
	tree.was( deletedInside, "inner/del.txt" );
	tree.was( a, "a.txt" );
	tree.was( c, "c.txt" );
	tree.was( gone, "gone.txt" );
	tree.was( kept, "kept.txt" );
	tree.is( second, inner, "renamed" );
	tree.is( second, leaf, "renamed/leaf.txt" );
	tree.is( second, renamedFile, "renamed/y.txt" );
	tree.is( second, a, "b.txt" );
	tree.is( second, c, "renamed/c.txt" );
	tree.is( second, kept, "kept.txt" );
	tree.is( second, created, "new.txt" );
	tree.move( first, second );
	runPass( batch, watches );

	struct Expected {
		efsw::Action Action;
		std::string Name;
		std::string Old;
	};
	const std::vector<Expected> expected{
		{ efsw::Actions::Moved, "outer", "" },
		{ efsw::Actions::Delete, "del.txt", "" },
		{ efsw::Actions::Delete, "gone.txt", "" },
		{ efsw::Actions::Moved, "b.txt", slashes( "/dst/outer/a.txt" ) },
		{ efsw::Actions::Add, "new.txt", "" },
		{ efsw::Actions::Moved, "renamed", slashes( "/dst/outer/inner" ) },
		{ efsw::Actions::Moved, "c.txt", slashes( "/dst/outer/c.txt" ) },
		{ efsw::Actions::Moved, "y.txt", slashes( "/dst/outer/renamed/x.txt" ) } };
	auto events = listener.getEvents();
	ASSERT_EQ( expected.size(), events.size() );
	for ( size_t i = 0; i < expected.size(); i++ ) {
		EXPECT_EQ( expected[i].Action, std::get<0>( events[i] ) );
		EXPECT_TRUE( expected[i].Name == std::get<1>( events[i] ) );
		if ( i > 0 )
			EXPECT_TRUE( expected[i].Old == std::get<2>( events[i] ) );
	}
}

/// A watch whose entries arrive after its first scan of the pass: only a rescan reports them.
class LateWatch : public FakeWatch {
  public:
	std::vector<Pending> Late;

	using FakeWatch::FakeWatch;

	void watch() override {
		FakeWatch::watch();
		Next.swap( Late );
	}

	void reportLate( const std::string& directory, const std::string& filename, efsw::Action action,
					 const efsw::FileInfo& info ) {
		report( directory, filename, action, info );
		Late.push_back( Next.back() );
		Next.pop_back();
	}
};

// A directory moved to a watch whose scan renamed files inside it, the halves of a swap seen
// at different moments: the comparison of its trees reports the swap, and the watch's own
// rename, from a name the listener never saw, is not reported too.
UTEST( FileActionBatch, RenameFoundInsideAComparedTreeIsItsComparison ) {
	START_BATCH_TEST();
	MovedTree tree( file );
	efsw::FileInfo a = file.make( "outer/a" );
	efsw::FileInfo b = file.make( "outer/b" );
	tree.was( a, "a" );
	tree.was( b, "b" );
	// Swapped through t: a to t, b to a, t to b.
	tree.is( second, b, "a" );
	tree.is( second, a, "b" );
	tree.move( first, second );
	second.report( "/dst/outer/", "b", efsw::Actions::Moved, a );
	second.Next.back().Old = "t";
	runPass( batch, watches );

	struct Expected {
		efsw::Action Action;
		std::string Name;
		std::string Old;
	};
	const std::vector<Expected> expected{ { efsw::Actions::Moved, "outer", "" },
										  { efsw::Actions::Delete, "b", "" },
										  { efsw::Actions::Moved, "b", slashes( "/dst/outer/a" ) },
										  { efsw::Actions::Add, "a", "" } };
	auto events = listener.getEvents();
	ASSERT_EQ( expected.size(), events.size() );
	for ( size_t i = 0; i < expected.size(); i++ ) {
		EXPECT_EQ( expected[i].Action, std::get<0>( events[i] ) );
		EXPECT_TRUE( expected[i].Name == std::get<1>( events[i] ) );
		// The directory's source is absolute, with a drive on Windows: not compared.
		if ( i > 0 )
			EXPECT_TRUE( expected[i].Old == std::get<2>( events[i] ) );
	}
}

// A directory moved out of a moved one to a watch scanned before the destination, after that
// scan: the tree lost it, so the watches scanned before are scanned again, and find it in the same
// pass. With no Delete of its own to pair with, it is reported as deleted from the tree, then
// added with what it holds, as a directory that left a compared tree.
UTEST( FileActionBatch, EntryMovedOutOfAMovedTreeToAnEarlierWatch ) {
	START_BATCH_TEST();
	LateWatch other( 4, batch, &listener );
	MovedTree tree( file );
	efsw::FileInfo inner = file.make( "outer/inner", true );
	efsw::FileInfo leaf = file.make( "outer/inner/file" );
	efsw::FileInfo kept = file.make( "outer/kept" );
	tree.was( inner, "inner" );
	tree.was( leaf, "inner/file" );
	tree.was( kept, "kept" );
	tree.is( second, kept, "kept" );
	tree.move( first, second );
	other.reportLate( "/other/", "inner", efsw::Actions::Add, inner );
	other.Tree.push_back( at( inner, "/other/inner" ) );
	other.Tree.push_back( at( leaf, "/other/inner/file" ) );
	runPass( batch, { &other, &second, &first } );
	runPass( batch, { &other, &second, &first } );

	// Its scan of each pass, and the rescan.
	EXPECT_EQ( 3, other.Scans );
	struct Expected {
		efsw::Action Action;
		std::string Name;
	};
	const std::vector<Expected> expected{ { efsw::Actions::Moved, "outer" },
										  { efsw::Actions::Delete, "file" },
										  { efsw::Actions::Delete, "inner" },
										  { efsw::Actions::Add, "inner" },
										  { efsw::Actions::Add, "file" } };
	auto events = listener.getEvents();
	ASSERT_EQ( expected.size(), events.size() );
	for ( size_t i = 0; i < expected.size(); i++ ) {
		EXPECT_EQ( expected[i].Action, std::get<0>( events[i] ) );
		EXPECT_TRUE( expected[i].Name == std::get<1>( events[i] ) );
	}
}

// A directory moved to a watch whose earlier Delete holds it to the next pass, where its scan
// finds a file of the directory renamed out of it: the held move is compared with the tree it
// arrived with, and the rename follows it, reported once.
UTEST( FileActionBatch, HeldMoveIsComparedWithTheTreeItArrivedWith ) {
	START_BATCH_TEST();
	MovedTree tree( file );
	efsw::FileInfo leaf = file.make( "outer/f" );
	efsw::FileInfo unrelated = file.make( "unrelated" );
	tree.was( leaf, "f" );
	tree.is( second, leaf, "f" );
	first.report( "/src/outer/", "f", efsw::Actions::Delete, leaf );
	second.report( "/dst/", "unrelated", efsw::Actions::Delete, unrelated );
	tree.move( first, second );
	runPass( batch, watches );
	EXPECT_EQ( 0u, listener.getEventCount() );

	// The file renamed out of the directory before the next scan.
	second.Tree.clear();
	second.report( "/dst/", "n5", efsw::Actions::Add, leaf );
	second.report( "/dst/outer/", "f", efsw::Actions::Delete, leaf );
	runPass( batch, watches );

	auto events = listener.getEvents();
	size_t deletes = 0;
	for ( const auto& event : events )
		if ( efsw::Actions::Delete == std::get<0>( event ) && std::get<1>( event ) == "f" )
			deletes++;
	EXPECT_EQ( 1u, deletes );
	ASSERT_EQ( 4u, events.size() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Delete, "unrelated" ) );
	EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Moved, "outer" ) );
	// The watch's own events, as it collected them.
	EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Add, "n5" ) );
	EXPECT_EQ( 3, listener.indexOf( efsw::Actions::Delete, "f" ) );
}

// A directory moved, then a file moved out of it to another watch: the file's Moved event comes
// after the directory's, so it is moved from where that event put it. When the directory's new
// watch does not watch inside it, the file arrives from outside the watched directories: an Add,
// with no Delete, since the directory's move took it away.
UTEST( FileActionBatch, MoveOutOfAMovedDirectoryIsFromItsNewPath ) {
	for ( bool watchesTrees : { true, false } ) {
		START_BATCH_TEST();
		efsw::FileInfo directory = file.make( "d", true );
		efsw::FileInfo inside = file.make( "d/f" );
		second.WatchesTrees = watchesTrees;
		first.report( "/a/", "d", efsw::Actions::Delete, directory );
		first.report( "/a/d/", "f", efsw::Actions::Delete, inside );
		second.report( "/", "d", efsw::Actions::Add, directory );
		third.report( "/c/", "n7", efsw::Actions::Add, inside );
		runPass( batch, watches );

		auto events = listener.getEvents();
		ASSERT_EQ( 2u, events.size() );
		EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
		EXPECT_TRUE( std::get<1>( events[0] ) == "d" );
		EXPECT_TRUE( std::get<1>( events[1] ) == "n7" );
		if ( watchesTrees ) {
			EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[1] ) );
			EXPECT_TRUE( std::get<2>( events[1] ) == slashes( "/d/f" ) );
		} else {
			EXPECT_EQ( efsw::Actions::Add, std::get<0>( events[1] ) );
		}
	}
}

// The same, with the file renamed inside the directory: its watch's rename follows the held
// move, from the name the move arrived with, which the comparison does not report again.
UTEST( FileActionBatch, HeldMoveIsFollowedByARenameInsideIt ) {
	START_BATCH_TEST();
	MovedTree tree( file );
	efsw::FileInfo leaf = file.make( "outer/f" );
	efsw::FileInfo unrelated = file.make( "unrelated" );
	tree.was( leaf, "f" );
	tree.is( second, leaf, "f" );
	first.report( "/src/outer/", "f", efsw::Actions::Delete, leaf );
	second.report( "/dst/", "unrelated", efsw::Actions::Delete, unrelated );
	tree.move( first, second );
	runPass( batch, watches );
	EXPECT_EQ( 0u, listener.getEventCount() );

	second.Tree.clear();
	tree.is( second, leaf, "g" );
	second.report( "/dst/outer/", "g", efsw::Actions::Moved, leaf );
	second.Next.back().Old = "f";
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 3u, events.size() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Delete, "unrelated" ) );
	EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Moved, "outer" ) );
	EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Moved, "g" ) );
	EXPECT_TRUE( std::get<2>( events[2] ) == "f" );
}

// The same, with a file added inside the directory: its watch's Add follows the held move.
UTEST( FileActionBatch, HeldMoveIsFollowedByAnAddInsideIt ) {
	START_BATCH_TEST();
	MovedTree tree( file );
	efsw::FileInfo leaf = file.make( "outer/f" );
	efsw::FileInfo added = file.make( "outer/new" );
	efsw::FileInfo unrelated = file.make( "unrelated" );
	tree.was( leaf, "f" );
	tree.is( second, leaf, "f" );
	second.report( "/dst/", "unrelated", efsw::Actions::Delete, unrelated );
	tree.move( first, second );
	runPass( batch, watches );
	EXPECT_EQ( 0u, listener.getEventCount() );

	tree.is( second, added, "new" );
	second.report( "/dst/outer/", "new", efsw::Actions::Add, added );
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 3u, events.size() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Delete, "unrelated" ) );
	EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Moved, "outer" ) );
	EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Add, "new" ) );
}

// A Delete inside a directory, held to the next pass, where the directory moves to another
// watch: it happened before the move, and is delivered before it, at the path it had.
UTEST( FileActionBatch, HeldDeleteInsideADirectoryMovedLater ) {
	START_BATCH_TEST();
	efsw::FileInfo directory = file.make( "d", true );
	efsw::FileInfo gone = file.make( "d/gone" );
	efsw::FileInfo unrelated = file.make( "unrelated" );
	second.report( "/s/", "unrelated", efsw::Actions::Delete, unrelated );
	second.report( "/s/d/", "gone", efsw::Actions::Delete, gone );
	runPass( batch, watches );
	EXPECT_EQ( 0u, listener.getEventCount() );

	second.report( "/s/", "d", efsw::Actions::Delete, directory );
	third.report( "/t/", "d", efsw::Actions::Add, directory );
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 3u, events.size() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Delete, "unrelated" ) );
	EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Delete, "gone" ) );
	EXPECT_TRUE( std::get<0>( events[1] ) == efsw::Actions::Delete );
	EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Moved, "d" ) );
}

// Two held moves compared in one pass, each with renames inside it since it arrived, the first
// with an entry renamed before the move too: the renamed entries of both comparisons stay valid
// until the pass is delivered.
UTEST( FileActionBatch, TwoHeldMovesWithRenamesInOnePass ) {
	START_BATCH_TEST();
	efsw::FileInfo o1 = file.make( "o1", true );
	efsw::FileInfo o2 = file.make( "o2", true );
	efsw::FileInfo a = file.make( "o1/a" );
	efsw::FileInfo b = file.make( "o2/b" );
	efsw::FileInfo c = file.make( "o2/c" );
	efsw::FileInfo unrelated = file.make( "unrelated" );
	std::vector<efsw::FileInfoList> before1( 1 ), before2( 1 );
	before1[0].push_back( at( a, "/src/o1/e" ) );
	before2[0].push_back( at( b, "/src/o2/e2" ) );
	before2[0].push_back( at( c, "/src/o2/h2" ) );
	second.Tree = { at( a, "/dst/o1/f" ), at( b, "/dst/o2/f2" ), at( c, "/dst/o2/i2" ) };
	// Held by the Delete before the moves in their destination's watch.
	second.report( "/dst/", "unrelated", efsw::Actions::Delete, unrelated );
	first.report( "/src/", "o1", efsw::Actions::Delete, o1 );
	first.GiveUp.emplace_back( at( o1, "/src/o1" ), std::move( before1 ) );
	first.report( "/src/", "o2", efsw::Actions::Delete, o2 );
	first.GiveUp.emplace_back( at( o2, "/src/o2" ), std::move( before2 ) );
	second.report( "/dst/", "o1", efsw::Actions::Add, o1 );
	second.report( "/dst/", "o2", efsw::Actions::Add, o2 );
	runPass( batch, watches );
	EXPECT_EQ( 0u, listener.getEventCount() );

	second.Tree = { at( a, "/dst/o1/g" ), at( b, "/dst/o2/g2" ), at( c, "/dst/o2/j2" ) };
	second.report( "/dst/o1/", "g", efsw::Actions::Moved, a );
	second.Next.back().Old = "f";
	second.report( "/dst/o2/", "g2", efsw::Actions::Moved, b );
	second.Next.back().Old = "f2";
	second.report( "/dst/o2/", "j2", efsw::Actions::Moved, c );
	second.Next.back().Old = "i2";
	runPass( batch, watches );

	// Each move with its renames before the hold, from the tree it left, then the renames since,
	// as the watch found them.
	struct Expected {
		std::string Name;
		std::string Old;
	};
	const std::vector<Expected> expected{ { "unrelated", "" },
										  { "o1", "" },
										  { "f", slashes( "/dst/o1/e" ) },
										  { "o2", "" },
										  { "f2", slashes( "/dst/o2/e2" ) },
										  { "i2", slashes( "/dst/o2/h2" ) },
										  { "g", "f" },
										  { "g2", "f2" },
										  { "j2", "i2" } };
	auto events = listener.getEvents();
	ASSERT_EQ( expected.size(), events.size() );
	EXPECT_EQ( efsw::Actions::Delete, std::get<0>( events[0] ) );
	for ( size_t i = 1; i < expected.size(); i++ ) {
		EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[i] ) );
		EXPECT_TRUE( expected[i].Name == std::get<1>( events[i] ) );
		// The directories' sources are absolute, with a drive on Windows: not compared.
		if ( !expected[i].Old.empty() )
			EXPECT_TRUE( expected[i].Old == std::get<2>( events[i] ) );
	}
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

// A directory moved out of one deleted in the same pass, to a watch whose earlier Delete holds
// it to the next pass: the deleted directory is held with it, so that the move does not name a
// source the listener was told is gone, and the directory's Delete still waits for it.
UTEST( FileActionBatch, DeletedDirectoryWaitsForAHeldMoveOutOfIt ) {
	START_BATCH_TEST();
	efsw::FileInfo directory = file.make( "d", true );
	efsw::FileInfo moved = file.make( "d/n5", true );
	efsw::FileInfo unrelated = file.make( "unrelated" );
	first.report( "/a/", "d", efsw::Actions::Delete, directory );
	first.report( "/a/d/", "n5", efsw::Actions::Delete, moved );
	third.report( "/c/", "unrelated", efsw::Actions::Delete, unrelated );
	third.report( "/c/", "n5", efsw::Actions::Add, moved );
	runPass( batch, watches );
	EXPECT_EQ( -1, listener.indexOf( efsw::Actions::Delete, "d" ) );

	runPass( batch, watches );
	const int move = listener.indexOf( efsw::Actions::Moved, "n5" );
	const int deleted = listener.indexOf( efsw::Actions::Delete, "d" );
	EXPECT_LE( 0, move );
	EXPECT_LT( move, deleted );
	EXPECT_EQ( 3u, listener.getEventCount() );
}

// A directory moved, and a file moved out of it to a watch whose earlier Delete holds the file's
// move to the next pass: the directory's move waits with it. Delivered in one pass, the file's
// move follows the directory's Moved event, which reports it deleted from the tree, so it is
// added where it went, instead of a move from a source the listener was told is gone.
UTEST( FileActionBatch, MovedDirectoryWaitsForAHeldMoveOutOfIt ) {
	START_BATCH_TEST();
	MovedTree tree( file );
	efsw::FileInfo leaving = file.make( "outer/x" );
	efsw::FileInfo unrelated = file.make( "unrelated" );
	tree.was( leaving, "x" );
	first.report( "/src/outer/", "x", efsw::Actions::Delete, leaving );
	tree.move( first, second );
	third.report( "/other/", "unrelated", efsw::Actions::Delete, unrelated );
	third.report( "/other/", "x", efsw::Actions::Add, leaving );
	runPass( batch, watches );
	EXPECT_EQ( 0u, listener.getEventCount() );

	runPass( batch, watches );
	auto events = listener.getEvents();
	ASSERT_EQ( 4u, events.size() );
	EXPECT_EQ( 0, listener.indexOf( efsw::Actions::Moved, "outer" ) );
	EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Delete, "x" ) );
	EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Delete, "unrelated" ) );
	EXPECT_EQ( 3, listener.indexOf( efsw::Actions::Add, "x" ) );
}

// A directory moved to a watch scanned first, out of a directory its own watch found renamed:
// the move comes before that rename, so its source is named as the listener knows it, under the
// directory's old name.
UTEST( FileActionBatch, MoveBeforeTheRenameOfItsSourceDirectory ) {
	START_BATCH_TEST();
	efsw::FileInfo renamedDirectory = file.make( "n113", true );
	efsw::FileInfo moved = file.make( "n96", true );
	first.report( "/c/", "n96", efsw::Actions::Add, moved );
	second.report( "/b/", "n128", efsw::Actions::Moved, renamedDirectory );
	second.Next.back().Old = "n113";
	second.report( "/b/n128/n83/", "n96", efsw::Actions::Delete, moved );
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 2u, events.size() );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
	EXPECT_TRUE( std::get<1>( events[0] ) == "n96" );
	const std::string known = slashes( "/b/n113/n83/n96" );
	const std::string& old = std::get<2>( events[0] );
	// Absolute, with a drive on Windows.
	EXPECT_TRUE( old.size() >= known.size() &&
				 0 == old.compare( old.size() - known.size(), known.size(), known ) );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[1] ) );
	EXPECT_TRUE( std::get<1>( events[1] ) == "n128" );
}

// Two renames hold the source: a directory inside another, then the outer one. Both are undone,
// the latest first.
UTEST( FileActionBatch, MoveBeforeTwoRenamesOfItsSourceDirectories ) {
	START_BATCH_TEST();
	efsw::FileInfo inner = file.make( "inner", true );
	efsw::FileInfo outer = file.make( "outer", true );
	efsw::FileInfo moved = file.make( "x" );
	// In the platform's separators, as the backends report directories: the old paths are
	// built from them.
	first.report( slashes( "/c/" ), "x", efsw::Actions::Add, moved );
	second.report( slashes( "/w/b/" ), "d", efsw::Actions::Moved, inner );
	second.Next.back().Old = "c";
	second.report( slashes( "/w/" ), "e", efsw::Actions::Moved, outer );
	second.Next.back().Old = "b";
	second.report( slashes( "/w/e/d/" ), "x", efsw::Actions::Delete, moved );
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 3u, events.size() );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
	const std::string known = slashes( "/w/b/c/x" );
	const std::string& old = std::get<2>( events[0] );
	EXPECT_TRUE( old.size() >= known.size() &&
				 0 == old.compare( old.size() - known.size(), known.size(), known ) );
}

// As above, with the rename and the Delete reported under different IDs of one registration,
// as kqueue reports each directory with its own.
UTEST( FileActionBatch, MoveBeforeTheRenameOfItsSourceDirectoryInOneRegistration ) {
	START_BATCH_TEST();
	efsw::FileInfo renamedDirectory = file.make( "n113", true );
	efsw::FileInfo moved = file.make( "n96", true );
	first.report( "/c/", "n96", efsw::Actions::Add, moved );
	second.report( "/b/", "n128", efsw::Actions::Moved, renamedDirectory );
	second.Next.back().Old = "n113";
	second.report( "/b/n128/n83/", "n96", efsw::Actions::Delete, moved );
	second.Next.back().Id = 7;
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 2u, events.size() );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
	EXPECT_TRUE( std::get<1>( events[0] ) == "n96" );
	const std::string known = slashes( "/b/n113/n83/n96" );
	const std::string& old = std::get<2>( events[0] );
	EXPECT_TRUE( old.size() >= known.size() &&
				 0 == old.compare( old.size() - known.size(), known.size(), known ) );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[1] ) );
}

// The rename of the source's directory is held to the next pass with the events of its
// registration after a held one: the move, delivered now, names its source as the listener
// still knows it.
UTEST( FileActionBatch, MoveBeforeAHeldRenameOfItsSourceDirectory ) {
	START_BATCH_TEST();
	efsw::FileInfo gone = file.make( "gone" );
	efsw::FileInfo renamedDirectory = file.make( "old", true );
	efsw::FileInfo moved = file.make( "child", true );
	first.report( "/c/", "child", efsw::Actions::Add, moved );
	second.report( "/b/", "gone", efsw::Actions::Delete, gone );
	second.report( "/b/", "new", efsw::Actions::Moved, renamedDirectory );
	second.Next.back().Old = "old";
	second.report( "/b/new/sub/", "child", efsw::Actions::Delete, moved );
	runPass( batch, watches );
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 3u, events.size() );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
	EXPECT_TRUE( std::get<1>( events[0] ) == "child" );
	const std::string known = slashes( "/b/old/sub/child" );
	const std::string& old = std::get<2>( events[0] );
	EXPECT_TRUE( old.size() >= known.size() &&
				 0 == old.compare( old.size() - known.size(), known.size(), known ) );
	EXPECT_TRUE( std::get<1>( events[2] ) == "new" );
}

// A listener removes the source's registration before its directory rename is delivered: the
// listener never learns the new name, so the move delivered after it names its source as before.
UTEST( FileActionBatch, MoveAfterACanceledRenameOfItsSourceDirectory ) {
	START_BATCH_TEST();
	(void)watches;
	RemovingListener removerListener;
	FakeWatch remover( 4, batch, &removerListener );
	removerListener.Batch = &batch;
	removerListener.Removed = &second;
	efsw::FileInfo renamedDirectory = file.make( "n113", true );
	efsw::FileInfo moved = file.make( "n96", true );
	remover.report( "/other/", "other.txt", efsw::Actions::Modified, efsw::FileInfo() );
	second.report( "/b/", "n128", efsw::Actions::Moved, renamedDirectory );
	second.Next.back().Old = "n113";
	second.report( "/b/n128/n83/", "n96", efsw::Actions::Delete, moved );
	first.report( "/c/", "n96", efsw::Actions::Add, moved );
	runPass( batch, { &remover, &second, &first } );

	auto events = listener.getEvents();
	ASSERT_EQ( 1u, events.size() );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
	EXPECT_TRUE( std::get<1>( events[0] ) == "n96" );
	const std::string known = slashes( "/b/n113/n83/n96" );
	const std::string& old = std::get<2>( events[0] );
	EXPECT_TRUE( old.size() >= known.size() &&
				 0 == old.compare( old.size() - known.size(), known.size(), known ) );
}

// The source's evidence of a tree may be split: a generic watch that found its directory gone
// emptied its snapshot and reported a Delete for each entry, while others gave up their
// snapshots. Both count, once each.
UTEST( FileActionBatch, TreeEvidenceFromSnapshotsAndDeletes ) {
	START_BATCH_TEST();
	MovedTree tree( file );
	efsw::FileInfo inner = file.make( "outer/inner", true );
	efsw::FileInfo leaf = file.make( "outer/inner/leaf.txt" );
	efsw::FileInfo top = file.make( "outer/top.txt" );
	tree.was( inner, "inner" );
	tree.was( top, "top.txt" );
	tree.is( second, inner, "renamed" );
	tree.is( second, leaf, "renamed/leaf.txt" );
	tree.is( second, top, "top.txt" );
	// Reported as Deletes, and also in the snapshot given up.
	first.report( "/src/outer/inner/", "leaf.txt", efsw::Actions::Delete, leaf );
	first.report( "/src/outer/", "top.txt", efsw::Actions::Delete, top );
	tree.move( first, second );
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 2u, events.size() );
	EXPECT_TRUE( std::get<1>( events[0] ) == "outer" );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[1] ) );
	EXPECT_TRUE( std::get<1>( events[1] ) == "renamed" );
}

// A directory moved out of one that was then deleted, to a watch scanned before: the deleted
// directory's Delete is held to the next pass, and its tree is still the evidence of the moved
// one, so what was created inside the moved directory follows its Moved event.
UTEST( FileActionBatch, TreeEvidenceOfAHeldDelete ) {
	START_BATCH_TEST();
	efsw::FileInfo parent = file.make( "p", true );
	efsw::FileInfo moved = file.make( "p/m", true );
	efsw::FileInfo created = file.make( "p/m/new.txt" );
	std::vector<efsw::FileInfoList> before( 1 );
	before[0].push_back( at( moved, "/src/p/m" ) );
	third.report( "/src/", "p", efsw::Actions::Delete, parent );
	third.report( "/src/p/", "m", efsw::Actions::Delete, moved );
	third.GiveUp.emplace_back( at( parent, "/src/p" ), std::move( before ) );
	first.report( "/dst/", "m", efsw::Actions::Add, moved );
	first.Tree.push_back( at( created, "/dst/m/new.txt" ) );

	runPass( batch, watches );
	auto events = listener.getEvents();
	ASSERT_EQ( 2u, events.size() );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
	EXPECT_TRUE( std::get<1>( events[0] ) == "m" );
	EXPECT_EQ( efsw::Actions::Add, std::get<0>( events[1] ) );
	EXPECT_TRUE( std::get<1>( events[1] ) == "new.txt" );

	runPass( batch, watches );
	EXPECT_EQ( 3u, listener.getEventCount() );
	EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Delete, "p" ) );
}

// Hard links share an identity: the links whose paths match are kept, and a single remaining
// one on each end is a move.
UTEST( FileActionBatch, HardLinksInsideAMovedTree ) {
	START_BATCH_TEST();
	MovedTree tree( file );
	efsw::FileInfo linked = file.make( "outer/linked.txt" );
	tree.was( linked, "a.txt" );
	tree.was( linked, "h.txt" );
	tree.is( second, linked, "a.txt" );
	tree.is( second, linked, "h2.txt" );
	tree.move( first, second );
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 2u, events.size() );
	EXPECT_TRUE( std::get<1>( events[0] ) == "outer" );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[1] ) );
	EXPECT_TRUE( std::get<1>( events[1] ) == "h2.txt" );
	EXPECT_TRUE( std::get<2>( events[1] ) == slashes( "/dst/outer/h.txt" ) );
}

// The evidence is only used to compare the trees of a matched move: a directory that left the
// watches, a destination that does not watch inside the directory, or a later pass, never report
// it on its own.
UTEST( FileActionBatch, EvidenceIsNeverReportedOnItsOwn ) {
	START_BATCH_TEST();
	efsw::FileInfo leaf = file.make( "leaf.txt" );
	{
		// Left every watch.
		MovedTree tree( file );
		tree.was( leaf, "leaf.txt" );
		first.report( "/src/", "outer", efsw::Actions::Delete, tree.Outer );
		first.GiveUp.emplace_back( at( tree.Outer, "/src/outer" ), std::move( tree.Before ) );
		runPass( batch, watches );
		auto events = listener.getEvents();
		ASSERT_EQ( 1u, events.size() );
		EXPECT_EQ( efsw::Actions::Delete, std::get<0>( events[0] ) );
		// It arrives a pass later: the evidence is gone.
		listener.clearEvents();
		tree.is( second, leaf, "renamed.txt" );
		second.report( "/dst/", "outer", efsw::Actions::Add, tree.Outer );
		runPass( batch, watches );
		events = listener.getEvents();
		ASSERT_EQ( 1u, events.size() );
		EXPECT_EQ( efsw::Actions::Add, std::get<0>( events[0] ) );
		second.Tree.clear();
	}
	{
		// The destination does not watch inside it.
		listener.clearEvents();
		MovedTree tree( file );
		tree.was( leaf, "leaf.txt" );
		tree.is( second, leaf, "renamed.txt" );
		second.WatchesTrees = false;
		tree.move( first, second );
		runPass( batch, watches );
		auto events = listener.getEvents();
		ASSERT_EQ( 1u, events.size() );
		EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
		second.WatchesTrees = true;
		second.Tree.clear();
	}
}

namespace {

/// A tree whose file was renamed after the move, delivered to a destination listener that
/// removes a registration on the Moved event of the directory.
/// @param destination Whether it removes its own registration, or the source's.
void runRemovalDuringTree( bool destination, MovedFile& file, int* utest_result ) {
	efsw::FileActionBatch batch;
	TestListener sourceListener;
	RemovingListener destinationListener;
	FakeWatch source( 1, batch, &sourceListener );
	FakeWatch target( 2, batch, &destinationListener );
	destinationListener.Batch = &batch;
	destinationListener.Removed = destination ? static_cast<efsw::Watcher*>( &target ) : &source;
	MovedTree tree( file );
	efsw::FileInfo leaf = file.make( "outer/leaf" + std::to_string( destination ) + ".txt" );
	tree.was( leaf, "leaf.txt" );
	tree.is( target, leaf, "renamed.txt" );
	tree.move( source, target );
	runPass( batch, { &source, &target } );

	auto events = destinationListener.getEvents();
	ASSERT_EQ( destination ? 1u : 2u, events.size() );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[0] ) );
	EXPECT_TRUE( std::get<1>( events[0] ) == "outer" );
	if ( !destination )
		EXPECT_TRUE( std::get<1>( events[1] ) == "renamed.txt" );
	EXPECT_EQ( 0u, sourceListener.getEventCount() );
}

} // namespace

// A listener removes the destination's registration on the directory's Moved event: nothing more
// is delivered to it. Removing the source's instead changes nothing for the destination.
UTEST( FileActionBatch, RemovalDuringATreeStopsItsFollowers ) {
	START_BATCH_TEST();
	(void)watches;
	runRemovalDuringTree( true, file, utest_result );
	runRemovalDuringTree( false, file, utest_result );
}

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

/// A tree below a moved directory: paths below it, starting with a separator, by identity.
typedef std::map<std::string, std::pair<efsw::FileIdentity, bool>> PlainTree;

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

bool hasIdentity( const PlainTree& tree, const efsw::FileIdentity& identity ) {
	for ( const auto& entry : tree )
		if ( entry.second.first == identity )
			return true;
	return false;
}

/// How the fake watches report entries moving between the tree and other.
enum class Reporting {
	/// As generic watches: the source reports a Delete for each entry that left, and other an
	/// Add for each entry that arrived.
	Entries,
	/// As kqueue watches: the source reports none of the entries of a tree that left, and
	/// other reports the entries that arrived in its directories, and the directories inside a
	/// directory that arrived, not its files.
	Directories
};

/// Moves outer, with the tree before, from the first watch to the second, which finds the tree
/// after; replays the events delivered, and checks that they turn one tree into the other.
/// Entries may move between the tree and a third watch, other, which has its own trees.
/// @param order The order in which the watches are scanned: 0 for the source, 1 for the
/// destination, 2 for other.
void checkReplayInOrder( const PlainTree& before, const PlainTree& after, int* utest_result,
						 const std::string& scenario, const PlainTree& otherBefore,
						 const PlainTree& otherAfter, Reporting reporting,
						 const std::array<int, 3>& order ) {
	const std::string slash = ReplayTree::slash();
	const std::string source = slash + "src" + slash + "outer";
	const std::string destination = slash + "dst" + slash + "outer";
	const std::string other = slash + "other";
	const efsw::FileIdentity outer( 1, 1 );
	efsw::FileActionBatch batch;
	PathListener listener;
	FakeWatch first( 1, batch, &listener );
	FakeWatch second( 2, batch, &listener );
	FakeWatch third( 3, batch, &listener );
	std::vector<efsw::FileInfoList> lists( 1 );
	ReplayTree replay;
	replay.Entries[slash + "src"] = Unknown;
	replay.Entries[slash + "dst"] = Unknown;
	replay.Entries[other] = Unknown;
	replay.Entries[source] = outer;
	for ( const auto& entry : before ) {
		lists[0].push_back(
			synthetic( source + entry.first, entry.second.first, entry.second.second ) );
		replay.Entries[source + entry.first] = replayed( entry.second );
		if ( Reporting::Entries == reporting && hasIdentity( otherAfter, entry.second.first ) &&
			 !hasIdentity( otherBefore, entry.second.first ) )
			reportAt( first, source + entry.first, efsw::Actions::Delete, entry.second );
	}
	for ( const auto& entry : after ) {
		second.Tree.push_back(
			synthetic( destination + entry.first, entry.second.first, entry.second.second ) );
		if ( hasIdentity( otherBefore, entry.second.first ) &&
			 !hasIdentity( otherAfter, entry.second.first ) )
			reportAt( second, destination + entry.first, efsw::Actions::Add, entry.second );
	}
	for ( const auto& entry : otherBefore ) {
		replay.Entries[other + entry.first] = replayed( entry.second );
		if ( hasIdentity( otherAfter, entry.second.first ) )
			continue;
		reportAt( third, other + entry.first, efsw::Actions::Delete, entry.second );
		// A directory that left other gives up its tree, as the polling backends' watches do.
		if ( entry.second.second ) {
			std::vector<efsw::FileInfoList> tree( 1 );
			const std::string below = entry.first + slash;
			for ( const auto& inside : otherBefore )
				if ( 0 == inside.first.compare( 0, below.size(), below ) )
					tree[0].push_back( synthetic( other + inside.first, inside.second.first,
												  inside.second.second ) );
			third.GiveUp.emplace_back( synthetic( other + entry.first, entry.second.first, true ),
									   std::move( tree ) );
		}
	}
	for ( const auto& entry : otherAfter ) {
		third.Tree.push_back(
			synthetic( other + entry.first, entry.second.first, entry.second.second ) );
		const std::string parent = entry.first.substr( 0, entry.first.rfind( slash[0] ) );
		const bool parentExisted = parent.empty() || otherBefore.count( parent );
		if ( !hasIdentity( otherBefore, entry.second.first ) &&
			 ( Reporting::Entries == reporting || entry.second.second || parentExisted ) )
			reportAt( third, other + entry.first, efsw::Actions::Add, entry.second );
	}
	first.report( slash + "src" + slash, "outer", efsw::Actions::Delete,
				  synthetic( source, outer, true ) );
	first.GiveUp.emplace_back( synthetic( source, outer, true ), std::move( lists ) );
	second.report( slash + "dst" + slash, "outer", efsw::Actions::Add,
				   synthetic( destination, outer, true ) );
	// Twice: a pass may hold a Delete to the next one.
	FakeWatch* watchers[3] = { &first, &second, &third };
	const std::vector<FakeWatch*> scans{ watchers[order[0]], watchers[order[1]],
										 watchers[order[2]] };
	runPass( batch, scans );
	runPass( batch, scans );

	std::string problem;
	for ( const auto& event : listener.Events ) {
		problem = replay.apply( event );
		if ( !problem.empty() )
			break;
	}
	if ( problem.empty() ) {
		std::map<std::string, efsw::FileIdentity> expected;
		expected[slash + "src"] = Unknown;
		expected[slash + "dst"] = Unknown;
		expected[other] = Unknown;
		expected[destination] = outer;
		for ( const auto& entry : after )
			expected[destination + entry.first] = replayed( entry.second );
		for ( const auto& entry : otherAfter )
			expected[other + entry.first] = replayed( entry.second );
		if ( expected.size() != replay.Entries.size() ) {
			problem = "the replayed tree has " + std::to_string( replay.Entries.size() ) +
					  " entries instead of " + std::to_string( expected.size() );
		} else {
			for ( const auto& entry : expected ) {
				auto found = replay.Entries.find( entry.first );
				if ( found == replay.Entries.end() ) {
					problem = "the replayed tree misses " + entry.first;
					break;
				}
				if ( found->second != Unknown && found->second != entry.second ) {
					problem = "the replayed tree has another entry at " + entry.first;
					break;
				}
			}
		}
	}
	if ( !problem.empty() ) {
		printf( "%s, scanned %d%d%d: %s\n", scenario.c_str(), order[0], order[1], order[2],
				problem.c_str() );
		auto dump = []( const char* label, const PlainTree& tree ) {
			for ( const auto& entry : tree )
				printf( "  %s %s #%llu%s\n", label, entry.first.c_str(),
						static_cast<unsigned long long>( entry.second.first.second ),
						entry.second.second ? " (directory)" : "" );
		};
		dump( "before", before );
		dump( "after", after );
		dump( "other before", otherBefore );
		dump( "other after", otherAfter );
		for ( const auto& event : listener.Events )
			printf( "  %d %s <- %s\n", (int)event.Action, event.Path.c_str(),
					event.OldPath.c_str() );
	}
	EXPECT_TRUE( problem.empty() );
}

/// checkReplayInOrder(), in every order in which the watches may be scanned.
void checkReplay( const PlainTree& before, const PlainTree& after, int* utest_result,
				  const std::string& scenario, const PlainTree& otherBefore = PlainTree(),
				  const PlainTree& otherAfter = PlainTree(),
				  Reporting reporting = Reporting::Entries ) {
	std::array<int, 3> order{ { 0, 1, 2 } };
	do {
		checkReplayInOrder( before, after, utest_result, scenario, otherBefore, otherAfter,
							reporting, order );
	} while ( std::next_permutation( order.begin(), order.end() ) );
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

} // namespace

// Changes that depend on one another inside a moved tree are delivered in an order a listener
// can apply: a directory replaced by a new one at its path, with its file moved into the new
// one; a chain of renames; swapped files; swapped directories; a directory deleted after an entry
// moved out of it; and a directory moved into a directory that moved.
UTEST( FileActionBatch, ChangesInsideAMovedTreeReplayInOrder ) {
	// inner renamed to z, a new inner created, and z/file moved into it.
	checkReplay( { { at( { "inner" } ), { id( 10 ), true } },
				   { at( { "inner", "file" } ), { id( 11 ), false } } },
				 { { at( { "z" } ), { id( 10 ), true } },
				   { at( { "inner" } ), { id( 12 ), true } },
				   { at( { "inner", "file" } ), { id( 11 ), false } } },
				 utest_result, "replaced directory" );
	// b renamed to c, then a to b.
	checkReplay( { { at( { "a" } ), { id( 10 ), false } }, { at( { "b" } ), { id( 11 ), false } } },
				 { { at( { "b" } ), { id( 10 ), false } }, { at( { "c" } ), { id( 11 ), false } } },
				 utest_result, "chain" );
	// a and b swapped.
	checkReplay( { { at( { "a" } ), { id( 10 ), false } }, { at( { "b" } ), { id( 11 ), false } } },
				 { { at( { "a" } ), { id( 11 ), false } }, { at( { "b" } ), { id( 10 ), false } } },
				 utest_result, "swapped files" );
	// Directories x and y swapped, with a file each.
	checkReplay( { { at( { "x" } ), { id( 10 ), true } },
				   { at( { "x", "f" } ), { id( 11 ), false } },
				   { at( { "y" } ), { id( 12 ), true } },
				   { at( { "y", "g" } ), { id( 13 ), false } } },
				 { { at( { "y" } ), { id( 10 ), true } },
				   { at( { "y", "f" } ), { id( 11 ), false } },
				   { at( { "x" } ), { id( 12 ), true } },
				   { at( { "x", "g" } ), { id( 13 ), false } } },
				 utest_result, "swapped directories" );
	// d/keep moved out, then d deleted with d/gone.
	checkReplay( { { at( { "d" } ), { id( 10 ), true } },
				   { at( { "d", "keep" } ), { id( 11 ), false } },
				   { at( { "d", "gone" } ), { id( 12 ), false } } },
				 { { at( { "keep" } ), { id( 11 ), false } } }, utest_result, "deleted directory" );
	// p renamed to q, then r moved into q.
	checkReplay(
		{ { at( { "p" } ), { id( 10 ), true } }, { at( { "r" } ), { id( 11 ), true } } },
		{ { at( { "q" } ), { id( 10 ), true } }, { at( { "q", "r" } ), { id( 11 ), true } } },
		utest_result, "into a moved directory" );
	// a moved into b, then b moved into a's old place: a/b swapped with nesting.
	checkReplay(
		{ { at( { "a" } ), { id( 10 ), true } }, { at( { "a", "b" } ), { id( 11 ), true } } },
		{ { at( { "b" } ), { id( 11 ), true } }, { at( { "b", "a" } ), { id( 10 ), true } } },
		utest_result, "inverted nesting" );
}

// An identity reused by an entry of another kind is another entry: a file deleted, and a
// directory created that reuses its inode, at the same path or another, or the other way round.
UTEST( FileActionBatch, ReusedIdentityOfAnotherKindInsideAMovedTree ) {
	checkReplay( { { at( { "item" } ), { id( 10 ), false } } },
				 { { at( { "item" } ), { id( 10 ), true } } }, utest_result,
				 "file replaced by a directory" );
	checkReplay( { { at( { "item" } ), { id( 10 ), true } },
				   { at( { "item", "inside" } ), { id( 11 ), false } } },
				 { { at( { "item" } ), { id( 10 ), false } } }, utest_result,
				 "directory replaced by a file" );
	checkReplay( { { at( { "a" } ), { id( 10 ), false } } },
				 { { at( { "b" } ), { id( 10 ), true } } }, utest_result,
				 "file deleted, directory created elsewhere" );
	checkReplay(
		{ { at( { "d" } ), { id( 10 ), true } }, { at( { "d", "f" } ), { id( 11 ), false } } },
		{ { at( { "f" } ), { id( 11 ), false } }, { at( { "d" } ), { id( 10 ), false } } },
		utest_result, "directory replaced by a file, its file moved out first" );
	checkReplay( { { at( { "x" } ), { id( 10 ), false } },
				   { at( { "y" } ), { id( 11 ), true } },
				   { at( { "y", "f" } ), { id( 12 ), false } } },
				 { { at( { "y" } ), { id( 10 ), true } },
				   { at( { "y", "f" } ), { id( 12 ), false } },
				   { at( { "x" } ), { id( 11 ), false } } },
				 utest_result, "kinds swapped between two identities" );
}

// The same for the other kinds of entries, whose file types tell them apart: a FIFO deleted and
// a socket created at its path that reuses its inode, and a FIFO deleted and a character device
// created at another path that reuses its inode. A FIFO kept in place is unchanged.
UTEST( FileActionBatch, ReusedIdentityOfAnotherSpecialKind ) {
	START_BATCH_TEST();
	MovedTree tree( file );
	// The file type bits, the same on every platform.
	const unsigned fifo = 010000, characterDevice = 020000, socket = 0140000;
	auto as = []( efsw::FileInfo info, unsigned type ) {
		info.Permissions = type | 0644;
		return info;
	};
	efsw::FileInfo reused = file.make( "outer/reused" );
	efsw::FileInfo moved = file.make( "outer/moved" );
	efsw::FileInfo kept = file.make( "outer/kept" );
	tree.was( as( reused, fifo ), "reused" );
	tree.was( as( moved, fifo ), "moved" );
	tree.was( as( kept, fifo ), "kept" );
	tree.is( second, as( reused, socket ), "reused" );
	tree.is( second, as( moved, characterDevice ), "elsewhere" );
	tree.is( second, as( kept, fifo ), "kept" );
	tree.move( first, second );
	runPass( batch, watches );

	const std::vector<std::pair<efsw::Action, std::string>> expected{
		{ efsw::Actions::Moved, "outer" },
		{ efsw::Actions::Delete, "reused" },
		{ efsw::Actions::Delete, "moved" },
		{ efsw::Actions::Add, "elsewhere" },
		{ efsw::Actions::Add, "reused" } };
	auto events = listener.getEvents();
	ASSERT_EQ( expected.size(), events.size() );
	for ( size_t i = 0; i < expected.size(); i++ ) {
		EXPECT_EQ( expected[i].first, std::get<0>( events[i] ) );
		EXPECT_TRUE( expected[i].second == std::get<1>( events[i] ) );
	}
}

namespace {

void changesAcrossAMovedTree( int* utest_result, Reporting reporting ) {
	auto checkReplay = [&]( const PlainTree& before, const PlainTree& after, int* result,
							const std::string& scenario, const PlainTree& otherBefore = PlainTree(),
							const PlainTree& otherAfter = PlainTree() ) {
		::checkReplay( before, after, result,
					   scenario + ( Reporting::Directories == reporting ? " (directories)" : "" ),
					   otherBefore, otherAfter, reporting );
	};
	// A file moved out to the other registration.
	checkReplay( { { at( { "file" } ), { id( 10 ), false } } }, {}, utest_result, "file out", {},
				 { { at( { "file" } ), { id( 10 ), false } } } );
	// A directory, with its file, moved out; the file inside a renamed one.
	checkReplay( { { at( { "d" } ), { id( 10 ), true } },
				   { at( { "d", "f" } ), { id( 11 ), false } },
				   { at( { "r" } ), { id( 12 ), true } },
				   { at( { "r", "g" } ), { id( 13 ), false } } },
				 { { at( { "r2" } ), { id( 12 ), true } } }, utest_result, "directory out", {},
				 { { at( { "d" } ), { id( 10 ), true } },
				   { at( { "d", "f" } ), { id( 11 ), false } },
				   { at( { "g" } ), { id( 13 ), false } } } );
	// A file moved out of a directory deleted after.
	checkReplay(
		{ { at( { "d" } ), { id( 10 ), true } }, { at( { "d", "f" } ), { id( 11 ), false } } }, {},
		utest_result, "out of a deleted directory", {},
		{ { at( { "f" } ), { id( 11 ), false } } } );
	// A file and a directory, with its file, moved in.
	checkReplay( { { at( { "in" } ), { id( 10 ), true } } },
				 { { at( { "in" } ), { id( 10 ), true } },
				   { at( { "in", "f" } ), { id( 11 ), false } },
				   { at( { "e" } ), { id( 12 ), true } },
				   { at( { "e", "g" } ), { id( 13 ), false } } },
				 utest_result, "into the tree",
				 { { at( { "f" } ), { id( 11 ), false } },
				   { at( { "e" } ), { id( 12 ), true } },
				   { at( { "e", "g" } ), { id( 13 ), false } } },
				 {} );
	// A directory moved out to other, while an unrelated Delete in other holds that move to the
	// next pass.
	checkReplay(
		{ { at( { "n0" } ), { id( 10 ), true } },
		  { at( { "n0", "n3" } ), { id( 13 ), false } },
		  { at( { "n2" } ), { id( 12 ), false } } },
		{ { at( { "m20" } ), { id( 12 ), false } } }, utest_result, "held move out",
		{ { at( { "o0" } ), { id( 14 ), false } }, { at( { "o3" } ), { id( 17 ), true } } },
		{ { at( { "o3" } ), { id( 17 ), true } },
		  { at( { "o3", "n0" } ), { id( 10 ), true } },
		  { at( { "o3", "n0", "n3" } ), { id( 13 ), false } } } );
	// A directory moved out with a directory renamed inside it.
	checkReplay( { { at( { "n0" } ), { id( 10 ), true } },
				   { at( { "n0", "n1" } ), { id( 11 ), true } },
				   { at( { "n0", "n1", "n2" } ), { id( 12 ), false } } },
				 {}, utest_result, "renamed inside a directory moved out", {},
				 { { at( { "n0" } ), { id( 10 ), true } },
				   { at( { "n0", "m19" } ), { id( 11 ), true } },
				   { at( { "n0", "m19", "n2" } ), { id( 12 ), false } } } );
	// A file moved into a directory, which then moves out.
	checkReplay(
		{ { at( { "n1" } ), { id( 11 ), false } }, { at( { "n2" } ), { id( 12 ), true } } }, {},
		utest_result, "into a directory moved out", {},
		{ { at( { "m20" } ), { id( 12 ), true } },
		  { at( { "m20", "m21" } ), { id( 11 ), false } } } );
	// A directory moved out, while entries inside it move out elsewhere.
	checkReplay( { { at( { "n0" } ), { id( 10 ), true } },
				   { at( { "n0", "n5" } ), { id( 15 ), true } },
				   { at( { "n0", "n6" } ), { id( 16 ), false } } },
				 {}, utest_result, "out of a directory moved out",
				 { { at( { "o5" } ), { id( 22 ), true } } },
				 { { at( { "n0" } ), { id( 10 ), true } },
				   { at( { "n6" } ), { id( 16 ), false } },
				   { at( { "o5" } ), { id( 22 ), true } },
				   { at( { "o5", "n5" } ), { id( 15 ), true } } } );
	// A directory moved in from other, a directory moved out of it into the tree, and a file
	// moved in from other into that one.
	checkReplay( { { at( { "n0" } ), { id( 10 ), true } } },
				 { { at( { "n0" } ), { id( 10 ), true } },
				   { at( { "i23" } ), { id( 19 ), true } },
				   { at( { "i23", "o1" } ), { id( 20 ), false } },
				   { at( { "n0", "m26" } ), { id( 22 ), true } },
				   { at( { "n0", "m26", "i24" } ), { id( 21 ), false } } },
				 utest_result, "through a directory moved in",
				 { { at( { "o0" } ), { id( 19 ), true } },
				   { at( { "o0", "o1" } ), { id( 20 ), false } },
				   { at( { "o0", "o3" } ), { id( 22 ), true } },
				   { at( { "o2" } ), { id( 21 ), false } } },
				 {} );
	// A directory moved out to other, with a file deleted inside it: whichever watch is
	// scanned first, its tree is compared.
	checkReplay( { { at( { "inner" } ), { id( 10 ), true } },
				   { at( { "inner", "gone" } ), { id( 11 ), false } } },
				 {}, utest_result, "changed inside a directory moved out", {},
				 { { at( { "inner" } ), { id( 10 ), true } } } );
	// A file moved into the tree from a directory of other, which is deleted after.
	checkReplay( {}, { { at( { "m25" } ), { id( 19 ), false } } }, utest_result,
				 "out of a deleted directory of other",
				 { { at( { "o0" } ), { id( 17 ), true } },
				   { at( { "o0", "o2" } ), { id( 19 ), false } },
				   { at( { "o1" } ), { id( 18 ), false } } },
				 { { at( { "o1" } ), { id( 18 ), false } } } );
	// A directory renamed, next to an entry whose name extends its own.
	checkReplay( { { at( { "inner" } ), { id( 10 ), true } },
				   { at( { "inner", "file" } ), { id( 11 ), false } },
				   { at( { "inner-x" } ), { id( 12 ), false } } },
				 { { at( { "renamed" } ), { id( 10 ), true } },
				   { at( { "renamed", "file" } ), { id( 11 ), false } },
				   { at( { "inner-x" } ), { id( 12 ), false } } },
				 utest_result, "prefix sibling" );
}

} // namespace

// Entries that move between the moved tree and another registration before the next scan, and
// names that extend others: the delivered events still turn every tree into what is on disk,
// whether the watches report each entry that moved, or only directories.
UTEST( FileActionBatch, ChangesAcrossAMovedTreeReplay ) {
	changesAcrossAMovedTree( utest_result, Reporting::Entries );
	changesAcrossAMovedTree( utest_result, Reporting::Directories );
}

// Random trees, changed by random renames, moves, deletions, creations and swaps: the delivered
// events always turn the tree before into the tree after.
UTEST( FileActionBatch, RandomChangesInsideAMovedTreeReplay ) {
	for ( unsigned seed = 1; seed <= 400; seed++ ) {
		std::mt19937 random( seed );
		auto pick = [&random]( size_t count ) {
			return std::uniform_int_distribution<size_t>( 0, count - 1 )( random );
		};
		unsigned long nextInode = 10;
		PlainTree tree;
		std::vector<std::string> directories{ "" };
		const int entries = 3 + static_cast<int>( pick( 12 ) );
		for ( int i = 0; i < entries; i++ ) {
			const bool directory = pick( 3 ) == 0;
			const std::string path = directories[pick( directories.size() )] + ReplayTree::slash() +
									 "n" + std::to_string( i );
			tree[path] = { id( nextInode++ ), directory };
			if ( directory )
				directories.push_back( path );
		}
		const PlainTree before = tree;

		auto parentOf = []( const std::string& path ) {
			return path.substr( 0, path.rfind( efsw::FileSystem::getOSSlash() ) );
		};
		auto isDirectory = [&tree]( const std::string& path ) {
			return path.empty() || ( tree.count( path ) && tree[path].second );
		};
		auto moveTree = [&tree]( const std::string& from, const std::string& to ) {
			PlainTree moved;
			for ( auto it = tree.begin(); it != tree.end(); ) {
				if ( it->first == from ||
					 0 == it->first.compare( 0, from.size() + 1, from + ReplayTree::slash() ) ) {
					moved[to + it->first.substr( from.size() )] = it->second;
					it = tree.erase( it );
				} else {
					++it;
				}
			}
			tree.insert( moved.begin(), moved.end() );
		};
		// The entries deleted so far, whose identities a new entry of the other kind may reuse.
		std::vector<std::pair<efsw::FileIdentity, bool>> deleted;
		const int changes = 1 + static_cast<int>( pick( 6 ) );
		for ( int c = 0; c < changes; c++ ) {
			std::vector<std::string> paths;
			for ( const auto& entry : tree )
				paths.push_back( entry.first );
			std::vector<std::string> targets{ "" };
			for ( const auto& entry : tree )
				if ( entry.second.second )
					targets.push_back( entry.first );
			const size_t kind = pick( 5 );
			if ( paths.empty() || kind == 3 ) {
				// Create, sometimes reusing a deleted entry's identity, with the other kind.
				const std::string path = targets[pick( targets.size() )] + ReplayTree::slash() +
										 "c" + std::to_string( nextInode );
				if ( !deleted.empty() && pick( 2 ) == 0 ) {
					const size_t reused = pick( deleted.size() );
					tree[path] = { deleted[reused].first, !deleted[reused].second };
					deleted.erase( deleted.begin() + reused );
					nextInode++;
					continue;
				}
				const bool directory = pick( 3 ) == 0;
				tree[path] = { id( nextInode++ ), directory };
				continue;
			}
			const std::string from = paths[pick( paths.size() )];
			if ( kind == 2 ) {
				// Delete, with what it holds.
				moveTree( from, ReplayTree::slash() + "deleted" );
				for ( auto it = tree.begin(); it != tree.end(); ) {
					if ( 0 == it->first.compare( 0, 8, ReplayTree::slash() + "deleted" ) ) {
						deleted.push_back( it->second );
						it = tree.erase( it );
					} else {
						++it;
					}
				}
				continue;
			}
			if ( kind == 4 ) {
				// Swap with another entry of the same kind, through a temporary name.
				const std::string other = paths[pick( paths.size() )];
				if ( other == from || tree[other].second != tree[from].second ||
					 0 == other.compare( 0, from.size() + 1, from + ReplayTree::slash() ) ||
					 0 == from.compare( 0, other.size() + 1, other + ReplayTree::slash() ) )
					continue;
				const std::string temporary = parentOf( from ) + ReplayTree::slash() + "tmp";
				if ( tree.count( temporary ) )
					continue;
				moveTree( from, temporary );
				moveTree( other, from );
				moveTree( temporary, other );
				continue;
			}
			// Rename or move, into a directory outside its own tree.
			const std::string into = kind == 0 ? parentOf( from ) : targets[pick( targets.size() )];
			if ( !isDirectory( into ) || into == from ||
				 0 == into.compare( 0, from.size() + 1, from + ReplayTree::slash() ) )
				continue;
			const std::string to = into + ReplayTree::slash() + "m" + std::to_string( nextInode++ );
			moveTree( from, to );
		}
		checkReplay( before, tree, utest_result, "seed " + std::to_string( seed ) );
	}
}

// Random trees in the moved directory and in another registration, with entries moved between
// them besides the changes inside the moved one: the delivered events turn both into what is on
// disk. The fake other watch reports entries arriving and leaving, not changes inside it.
UTEST( FileActionBatch, RandomChangesAcrossAMovedTreeReplay ) {
	const std::string slash = ReplayTree::slash();
	for ( unsigned seed = 1; seed <= 300; seed++ ) {
		std::mt19937 random( seed );
		auto pick = [&random]( size_t count ) {
			return std::uniform_int_distribution<size_t>( 0, count - 1 )( random );
		};
		unsigned long nextInode = 10;
		auto randomTree = [&]( PlainTree& tree, int prefix ) {
			std::vector<std::string> directories{ "" };
			const int entries = 2 + static_cast<int>( pick( 8 ) );
			for ( int i = 0; i < entries; i++ ) {
				const bool directory = pick( 3 ) == 0;
				const std::string path = directories[pick( directories.size() )] + slash +
										 ( prefix ? "o" : "n" ) + std::to_string( i );
				tree[path] = { id( nextInode++ ), directory };
				if ( directory )
					directories.push_back( path );
			}
		};
		PlainTree moved, other;
		randomTree( moved, 0 );
		randomTree( other, 1 );
		const PlainTree before = moved, otherBefore = other;

		// An entry crosses between the trees once at most: more would be a change inside other.
		std::set<efsw::FileIdentity> crossed;
		auto crosses = [&crossed]( const PlainTree& subtree ) {
			for ( const auto& entry : subtree )
				if ( crossed.count( entry.second.first ) )
					return false;
			for ( const auto& entry : subtree )
				crossed.insert( entry.second.first );
			return true;
		};
		auto take = []( PlainTree& tree, const std::string& from, const std::string& slashText ) {
			PlainTree subtree;
			for ( auto it = tree.begin(); it != tree.end(); ) {
				if ( it->first == from ||
					 0 == it->first.compare( 0, from.size() + 1, from + slashText ) ) {
					subtree[it->first.substr( from.size() )] = it->second;
					it = tree.erase( it );
				} else {
					++it;
				}
			}
			return subtree;
		};
		auto put = []( PlainTree& tree, const std::string& to, const PlainTree& subtree ) {
			for ( const auto& entry : subtree )
				tree[to + entry.first] = entry.second;
		};
		auto directoriesOf = []( const PlainTree& tree ) {
			std::vector<std::string> result{ "" };
			for ( const auto& entry : tree )
				if ( entry.second.second )
					result.push_back( entry.first );
			return result;
		};
		const int changes = 1 + static_cast<int>( pick( 6 ) );
		for ( int c = 0; c < changes; c++ ) {
			const size_t kind = pick( 6 );
			if ( kind == 0 || moved.empty() ) {
				// Created in the moved tree.
				auto targets = directoriesOf( moved );
				const std::string path =
					targets[pick( targets.size() )] + slash + "c" + std::to_string( nextInode );
				moved[path] = { id( nextInode++ ), pick( 3 ) == 0 };
				continue;
			}
			std::vector<std::string> paths;
			for ( const auto& entry : moved )
				paths.push_back( entry.first );
			if ( kind == 5 && !other.empty() ) {
				// From other into the moved tree, renamed.
				std::vector<std::string> otherPaths;
				for ( const auto& entry : other )
					otherPaths.push_back( entry.first );
				const std::string from = otherPaths[pick( otherPaths.size() )];
				auto targets = directoriesOf( moved );
				const std::string to =
					targets[pick( targets.size() )] + slash + "i" + std::to_string( nextInode++ );
				PlainTree subtree = take( other, from, slash );
				if ( crosses( subtree ) )
					put( moved, to, subtree );
				else
					put( other, from, subtree );
				continue;
			}
			const std::string from = paths[pick( paths.size() )];
			if ( kind == 1 ) {
				// Deleted, with what it holds.
				take( moved, from, slash );
				continue;
			}
			if ( kind == 4 ) {
				// Out to other, keeping its name, into a directory other had: other reports what
				// arrives there.
				std::vector<std::string> targets{ "" };
				for ( const auto& entry : other )
					if ( entry.second.second && hasIdentity( otherBefore, entry.second.first ) )
						targets.push_back( entry.first );
				const std::string to =
					targets[pick( targets.size() )] + from.substr( from.rfind( slash[0] ) );
				if ( other.count( to ) )
					continue;
				PlainTree subtree = take( moved, from, slash );
				// A directory holding entries no watch knew, as those created after the move,
				// arrives in other reported as a directory only, as one from anywhere else.
				bool known = true;
				for ( const auto& entry : subtree )
					known = known &&
							( subtree.size() == 1 || hasIdentity( before, entry.second.first ) );
				if ( known && crosses( subtree ) )
					put( other, to, subtree );
				else
					put( moved, from, subtree );
				continue;
			}
			// Renamed or moved inside the moved tree.
			auto targets = directoriesOf( moved );
			const std::string into = kind == 2 ? from.substr( 0, from.rfind( slash[0] ) )
											   : targets[pick( targets.size() )];
			if ( into == from || 0 == into.compare( 0, from.size() + 1, from + slash ) )
				continue;
			put( moved, into + slash + "m" + std::to_string( nextInode++ ),
				 take( moved, from, slash ) );
		}
		checkReplay( before, moved, utest_result, "seed " + std::to_string( seed ), otherBefore,
					 other );
		checkReplay( before, moved, utest_result,
					 "seed " + std::to_string( seed ) + " (directories)", otherBefore, other,
					 Reporting::Directories );
	}
}

namespace {

/// A directory moved out to other, held to the next pass by an unrelated Delete in other, with
/// its tree's move, which waits for it, and a listener removes a registration on that Moved
/// event: the source, the destination, or an unrelated one with held events of its own, which
/// come before it. Other still gets the directory.
/// @param removal 0: the source. 1: the destination. 2: the unrelated registration.
void runRemovalWithHeldMoves( int removal, int* utest_result ) {
	const std::string slash = ReplayTree::slash();
	efsw::FileActionBatch batch;
	TestListener sourceListener, otherListener, unrelatedListener, earlierListener;
	RemovingListener destinationListener;
	FakeWatch earlier( 5, batch, &earlierListener );
	FakeWatch unrelated( 4, batch, &unrelatedListener );
	FakeWatch source( 1, batch, &sourceListener );
	FakeWatch destination( 2, batch, &destinationListener );
	FakeWatch other( 3, batch, &otherListener );
	destinationListener.Batch = &batch;
	destinationListener.Removed = removal == 0	 ? static_cast<efsw::Watcher*>( &source )
								  : removal == 1 ? static_cast<efsw::Watcher*>( &destination )
												 : static_cast<efsw::Watcher*>( &unrelated );
	const std::pair<efsw::FileIdentity, bool> outer( id( 1 ), true ), n0( id( 10 ), true ),
		n3( id( 13 ), false ), gone( id( 14 ), false ), lost( id( 30 ), false ),
		seen( id( 31 ), false );
	const std::string src = slash + "src" + slash + "outer";
	const std::string dst = slash + "dst" + slash + "outer";
	const std::string oth = slash + "other";
	std::vector<efsw::FileInfoList> lists( 1 );
	lists[0].push_back( synthetic( src + slash + "n0", n0.first, true ) );
	lists[0].push_back( synthetic( src + slash + "n0" + slash + "n3", n3.first, false ) );
	reportAt( earlier, slash + "early" + slash + "seen", efsw::Actions::Add, seen );
	reportAt( unrelated, slash + "unrelated" + slash + "lost", efsw::Actions::Delete, lost );
	reportAt( source, src + slash + "n0", efsw::Actions::Delete, n0 );
	reportAt( source, src + slash + "n0" + slash + "n3", efsw::Actions::Delete, n3 );
	reportAt( source, src, efsw::Actions::Delete, outer );
	source.GiveUp.emplace_back( synthetic( src, outer.first, true ), std::move( lists ) );
	reportAt( destination, dst, efsw::Actions::Add, outer );
	reportAt( other, oth + slash + "gone", efsw::Actions::Delete, gone );
	reportAt( other, oth + slash + "n0", efsw::Actions::Add, n0 );
	reportAt( other, oth + slash + "n0" + slash + "n3", efsw::Actions::Add, n3 );
	other.Tree.push_back( synthetic( oth + slash + "n0", n0.first, true ) );
	other.Tree.push_back( synthetic( oth + slash + "n0" + slash + "n3", n3.first, false ) );
	const std::vector<FakeWatch*> order{ &earlier, &unrelated, &source, &destination, &other };
	runPass( batch, order );
	runPass( batch, order );

	EXPECT_NE( -1, destinationListener.indexOf( efsw::Actions::Moved, "outer" ) );
	EXPECT_TRUE( otherListener.checkEvent( efsw::Actions::Add, "n0" ) ||
				 otherListener.indexOf( efsw::Actions::Moved, "n0" ) != -1 );
	// The unrelated registration gets nothing after its removal: only its Delete, which came
	// before the Moved event.
	if ( removal == 2 ) {
		EXPECT_GE( 1u, unrelatedListener.getEventCount() );
		if ( 1u == unrelatedListener.getEventCount() )
			EXPECT_EQ( 0, unrelatedListener.indexOf( efsw::Actions::Delete, "lost" ) );
	}
}

} // namespace

// A listener removes a registration while the changes of a moved tree refer to held events:
// the held events stay where they are until the next pass, which still delivers what remains.
UTEST( FileActionBatch, RemovalWithHeldMovesOfATree ) {
	runRemovalWithHeldMoves( 0, utest_result );
	runRemovalWithHeldMoves( 1, utest_result );
	runRemovalWithHeldMoves( 2, utest_result );
}

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

// A directory moved in, held to the next pass, where its watch found a directory inside it
// renamed: compared with the tree it arrived with, which had the renamed directory's entries
// below its old name, only the rename follows the move.
UTEST( FileActionBatch, HeldMoveIsFollowedByARenameOfADirectoryInsideIt ) {
	efsw::FileActionBatch batch;
	TestListener listener;
	FakeWatch first( 1, batch, &listener );
	FakeWatch second( 2, batch, &listener );
	FakeWatch third( 3, batch, &listener );
	std::vector<FakeWatch*> watches{ &first, &second, &third };
	efsw::FileInfo outer = synthetic( slashes( "/src/outer" ), id( 1001 ), true );
	efsw::FileInfo inner = synthetic( slashes( "/src/outer/inner" ), id( 1002 ), true );
	efsw::FileInfo file = synthetic( slashes( "/src/outer/inner/f" ), id( 1003 ), false );
	second.Tree = { at( inner, "/dst/outer/inner" ), at( file, "/dst/outer/inner/f" ) };
	std::vector<efsw::FileInfoList> evidence( 1 );
	evidence[0] = { inner, file };
	first.GiveUp.emplace_back( outer, std::move( evidence ) );
	first.report( "/src/", "outer", efsw::Actions::Delete, outer );
	// Held: the destination's watch was scanned first, so the move waits for a rescan.
	second.report( "/dst/", "unrelated", efsw::Actions::Delete,
				   synthetic( "", id( 1004 ), false ) );
	second.report( "/dst/", "outer", efsw::Actions::Add, outer );
	runPass( batch, watches );
	EXPECT_EQ( 0u, listener.getEventCount() );

	second.Tree = { at( inner, "/dst/outer/renamed" ), at( file, "/dst/outer/renamed/f" ) };
	second.report( "/dst/outer/", "renamed", efsw::Actions::Moved, inner );
	second.Next.back().Old = "inner";
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 3u, events.size() );
	EXPECT_EQ( efsw::Actions::Delete, std::get<0>( events[0] ) );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[1] ) );
	EXPECT_TRUE( std::get<1>( events[1] ) == "outer" );
	EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[2] ) );
	EXPECT_TRUE( std::get<1>( events[2] ) == "renamed" );
	EXPECT_TRUE( std::get<2>( events[2] ) == "inner" );
}

// A directory left the watches, its Deletes held to the next pass with a file inside it. There,
// the file comes back, after a Delete that holds its watch again: the move would be held with
// it, out of a directory the listener was told is gone. It falls apart: the file's Delete is
// delivered with the directory, and the file arrives on the next pass, as an Add.
UTEST( FileActionBatch, MoveOutOfADeliveredDirectoryIsNotHeld ) {
	START_BATCH_TEST();
	efsw::FileInfo directory = file.make( "n10", true );
	efsw::FileInfo inner = file.make( "n10/d", true );
	efsw::FileInfo moved = file.make( "n10/d/f" );
	efsw::FileInfo gone = file.make( "n14" );
	second.report( "/b/", "n10", efsw::Actions::Delete, directory );
	second.report( "/b/n10/", "d", efsw::Actions::Delete, inner );
	second.report( "/b/n10/d/", "f", efsw::Actions::Delete, moved );
	runPass( batch, watches );
	EXPECT_EQ( 0u, listener.getEventCount() );

	second.report( "/b/", "n14", efsw::Actions::Delete, gone );
	second.report( "/b/n27/", "n44", efsw::Actions::Add, moved );
	runPass( batch, watches );
	runPass( batch, watches );

	auto events = listener.getEvents();
	ASSERT_EQ( 5u, events.size() );
	EXPECT_EQ( efsw::Actions::Delete, std::get<0>( events[0] ) );
	EXPECT_TRUE( std::get<1>( events[0] ) == "n10" );
	EXPECT_EQ( efsw::Actions::Delete, std::get<0>( events[2] ) );
	EXPECT_TRUE( std::get<1>( events[2] ) == "f" );
	EXPECT_EQ( efsw::Actions::Delete, std::get<0>( events[3] ) );
	EXPECT_TRUE( std::get<1>( events[3] ) == "n14" );
	EXPECT_EQ( efsw::Actions::Add, std::get<0>( events[4] ) );
	EXPECT_TRUE( std::get<1>( events[4] ) == "n44" );
}

// Two moves wait for each other: each registration's events wait behind a move into the other.
// The moves are split into their halves; one of them left a directory renamed after it, whose
// tree's changes then do not report it leaving a second time.
UTEST( FileActionBatch, SplitMoveOutOfAMovedTreeIsReportedOnce ) {
	START_BATCH_TEST();
	efsw::FileInfo directory = file.make( "p", true );
	efsw::FileInfo left = file.make( "p/m" );
	efsw::FileInfo kept = file.make( "p/k" );
	efsw::FileInfo crossing = file.make( "f" );
	std::vector<efsw::FileInfoList> before( 1 );
	before[0] = { at( left, "/a/p/m" ), at( kept, "/a/p/k" ) };
	first.GiveUp.emplace_back( at( directory, "/a/p" ), std::move( before ) );
	first.Tree = { at( kept, "/a/q/p/k" ) };
	first.report( "/a/", "p", efsw::Actions::Delete, directory );
	first.report( "/a/p/", "k", efsw::Actions::Delete, kept );
	first.report( "/a/p/", "m", efsw::Actions::Delete, left );
	first.report( "/a/q/", "p", efsw::Actions::Add, directory );
	first.report( "/a/q/", "g", efsw::Actions::Add, crossing );
	second.report( "/b/", "f", efsw::Actions::Delete, crossing );
	second.report( "/b/", "m", efsw::Actions::Add, left );
	runPass( batch, watches );

	int leaving = 0;
	for ( const auto& event : listener.getEvents() )
		if ( efsw::Actions::Delete == std::get<0>( event ) && std::get<1>( event ) == "m" )
			leaving++;
	EXPECT_EQ( 1, leaving );
	EXPECT_NE( -1, listener.indexOf( efsw::Actions::Add, "m" ) );
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

// A directory arrived twice in one pass, in two watches, after it left a third one, so its
// halves are not paired; a file inside it left for an unwatched directory. The third watch is
// held from the file's Delete, which has no Add; its directory's Delete, before it, is held too:
// on the next pass the file comes back to a watch, and its move from inside the directory comes
// before the listener is told the directory is gone.
UTEST( FileActionBatch, HoldInsideADeletedDirectoryHoldsTheDirectory ) {
	START_BATCH_TEST();
	efsw::FileInfo directory = file.make( "d", true );
	efsw::FileInfo inner = file.make( "d/f" );
	first.report( "/w1/", "n1", efsw::Actions::Add, directory );
	second.report( "/w2/", "n8", efsw::Actions::Add, directory );
	third.report( "/w3/", "d", efsw::Actions::Delete, directory );
	third.report( "/w3/d/", "f", efsw::Actions::Delete, inner );
	runPass( batch, watches );
	EXPECT_EQ( -1, listener.indexOf( efsw::Actions::Delete, "d" ) );

	second.report( "/w2/", "n12", efsw::Actions::Add, inner );
	runPass( batch, watches );
	runPass( batch, watches );

	const int moved = listener.indexOf( efsw::Actions::Moved, "n12" );
	const int deleted = listener.indexOf( efsw::Actions::Delete, "d" );
	EXPECT_NE( -1, moved );
	EXPECT_NE( -1, deleted );
	EXPECT_LT( moved, deleted );
}
