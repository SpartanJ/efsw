#include "test_util.hpp"
#include "utest.h"

#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_FSEVENTS

#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/FileWatcherFSEvents.hpp>
#include <efsw/WatcherFSEvents.hpp>
#include <mutex>
#include <string>
#include <vector>

using namespace efsw_test;

namespace {

/// An FSEvents watcher whose registrations pair the records of one callback given directly,
/// without its stream.
class PairingWatcher : public efsw::FileWatcherFSEvents {
  public:
	PairingWatcher() : FileWatcherFSEvents( nullptr ) {}

	void pair( const std::vector<efsw::FSEvent>& records ) {
		std::lock_guard<std::mutex> lock( mWatchesMutex );
		mWatchBuffer.clear();
		for ( const auto& watch : mWatches )
			mWatchBuffer.push_back( watch.second );
		mEventBuffer = records;
		pairCrossWatchMoves( records.size() );
	}

	const std::string& movedFrom( size_t record ) const { return mMovedFrom[record]; }

	bool claimed( size_t record ) const { return NULL != mClaimedBy[record]; }
};

/// Two registrations that both report moves between them, and a file in the second.
struct TwoRegistrations {
	std::string Source;
	std::string Destination;
	std::string File;
	efsw::Uint64 Inode{ 0 };
	TestListener Listener;
	PairingWatcher Watcher;

	TwoRegistrations() {
		const std::string base = getTemporaryDirectory();
		removeDirectory( base + "_pairing_a" );
		removeDirectory( base + "_pairing_b" );
		createDirectory( base + "_pairing_a" );
		createDirectory( base + "_pairing_b" );
		createFile( base + "_pairing_b/final", "x" );
		Source = efsw::FileSystem::getRealPath( base + "_pairing_a" ) + "/";
		Destination = efsw::FileSystem::getRealPath( base + "_pairing_b" ) + "/";
		File = Destination + "final";
		Inode = efsw::FileInfo( File ).Inode;
		const std::vector<efsw::WatcherOption> options = {
			{ efsw::Options::ReportCrossDirectoryMoves, 1 } };
		Watcher.addWatch( Source, &Listener, false, options );
		Watcher.addWatch( Destination, &Listener, false, options );
	}

	~TwoRegistrations() {
		removeDirectory( Source );
		removeDirectory( Destination );
	}

	efsw::FSEvent renamed( const std::string& path ) const {
		return efsw::FSEvent( path, efsw::efswFSEventStreamEventFlagItemRenamed, 0, Inode );
	}
};

} // namespace

// A file renamed twice in the first registration, then into the second: the first of its old
// paths is the move's source.
UTEST( FSEventsPairing, FirstSourcePairs ) {
	TwoRegistrations setup;
	setup.Watcher.pair( { setup.renamed( setup.Source + "n0" ),
						  setup.renamed( setup.Source + "n1" ), setup.renamed( setup.File ) } );

	EXPECT_TRUE( setup.Watcher.movedFrom( 2 ) == setup.Source + "n0" );
	EXPECT_TRUE( setup.Watcher.claimed( 0 ) );
	EXPECT_FALSE( setup.Watcher.claimed( 1 ) );
	EXPECT_TRUE( setup.Watcher.claimed( 2 ) );
}

// An old path in the destination's own registration cannot be a move between registrations:
// the next old path is the source.
UTEST( FSEventsPairing, SourceInTheSameRegistrationIsSkipped ) {
	TwoRegistrations setup;
	setup.Watcher.pair( { setup.renamed( setup.Destination + "old" ),
						  setup.renamed( setup.Source + "n0" ), setup.renamed( setup.File ) } );

	EXPECT_TRUE( setup.Watcher.movedFrom( 2 ) == setup.Source + "n0" );
	EXPECT_FALSE( setup.Watcher.claimed( 0 ) );
	EXPECT_TRUE( setup.Watcher.claimed( 1 ) );
}

// FSEvents may list the new path before the old one.
UTEST( FSEventsPairing, DestinationBeforeItsSource ) {
	TwoRegistrations setup;
	setup.Watcher.pair( { setup.renamed( setup.File ), setup.renamed( setup.Source + "n0" ),
						  setup.renamed( setup.Source + "n1" ) } );

	EXPECT_TRUE( setup.Watcher.movedFrom( 0 ) == setup.Source + "n0" );
	EXPECT_TRUE( setup.Watcher.claimed( 1 ) );
	EXPECT_FALSE( setup.Watcher.claimed( 2 ) );
}

// Many renames of one file that cannot pair, all inside one registration: each old path is
// tried once against the destination, instead of against every later record.
UTEST( FSEventsPairing, LongChainWithoutAPair ) {
	TwoRegistrations setup;
	std::vector<efsw::FSEvent> records;
	const size_t renames = 20000;
	for ( size_t i = 0; i < renames; i++ )
		records.push_back( setup.renamed( setup.Destination + "n" + std::to_string( i ) ) );
	records.push_back( setup.renamed( setup.File ) );
	setup.Watcher.pair( records );

	for ( size_t i = 0; i <= renames; i++ ) {
		EXPECT_FALSE( setup.Watcher.claimed( i ) );
		if ( setup.Watcher.claimed( i ) )
			break;
	}
	EXPECT_TRUE( setup.Watcher.movedFrom( renames ).empty() );
}

#endif
