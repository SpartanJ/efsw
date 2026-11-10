#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileActionBatch.hpp>
#include <efsw/FileInfo.hpp>

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
			Batch.add( ID, event.Directory, event.Filename, event.Action, "", event.Info, Listener );
		Next.clear();
	}

	void report( const std::string& directory, const std::string& filename, efsw::Action action,
				 efsw::FileInfo info ) {
		info.Filepath = directory + filename;
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
		Identity = efsw::FileIdentity( Info.Device, Info.Inode );
		return Info.Inode != 0;
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
	EXPECT_EQ( 1, listener.indexOf( efsw::Actions::Modified, "later.txt" ) );
	EXPECT_EQ( 2, listener.indexOf( efsw::Actions::Moved, "file.txt" ) );
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
