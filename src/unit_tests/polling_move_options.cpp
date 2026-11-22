#include "test_util.hpp"
#include "utest.h"
#include <efsw/FileWatcherGeneric.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE
#include <efsw/FileWatcherKqueue.hpp>
#include <efsw/System.hpp>
#endif

using namespace efsw_test;

namespace {

// Poll real registrations synchronously: no worker, sleeps or callback-driven removals.
struct PollingGeneric : efsw::FileWatcherGeneric {
	using FileWatcherGeneric::FileWatcherGeneric;

	void poll() {
		for ( auto* watch : mWatches ) {
			watch->watch();
		}
	}
};

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE
struct PollingKqueue : efsw::FileWatcherKqueue {
	using FileWatcherKqueue::FileWatcherKqueue;

	void poll() {
		for ( const auto& entry : mWatches ) {
			entry.second->watch();
		}
	}
};

struct PollingKqueueFallback : PollingKqueue {
	PollingKqueueFallback( efsw::FileWatcher* owner, unsigned int frequency ) :
		PollingKqueue( owner, frequency ) {
		// This fixture opens only generic roots, so no native descriptors are counted
		// while exhausted. No process-wide descriptor limit is changed.
		mFileDescriptorCount = static_cast<long>( efsw::System::getMaxFD() ) - 250;
	}
};
#endif

template <typename Backend>
void checkPollingMove( bool separateWatches, bool recursive, bool directory, bool enabled,
					   int* utest_result ) {
	const std::string root = getTemporaryDirectory() + "_polling_move_option";
	struct Cleanup {
		const std::string& Root;
		~Cleanup() { removeDirectory( Root ); }
	} cleanup{ root };
	ASSERT_TRUE( createDirectory( root ) );
	const std::string source = root + "/source";
	const std::string destination = root + "/destination";
	ASSERT_TRUE( createDirectory( source ) );
	ASSERT_TRUE( createDirectory( destination ) );
	if ( directory ) {
		ASSERT_TRUE( createDirectory( source + "/entry" ) );
		ASSERT_TRUE( createFile( source + "/entry/kept.txt", "x" ) );
	} else {
		ASSERT_TRUE( createFile( source + "/entry", "x" ) );
	}
	TestListener listener;
	efsw::FileWatcher owner( true );
	Backend backend( &owner, 100 );
	const std::vector<efsw::WatcherOption> options = {
		{ efsw::Options::ReportCrossDirectoryMoves, enabled ? 1 : 0 } };
	if ( separateWatches ) {
		ASSERT_GT( backend.addWatch( source, &listener, recursive, options ), 0 );
		ASSERT_GT( backend.addWatch( destination, &listener, recursive, options ), 0 );
	} else {
		ASSERT_GT( backend.addWatch( root, &listener, true, options ), 0 );
	}
	ASSERT_TRUE( renameFile( source + "/entry", destination + "/entry" ) );
	for ( int pass = 0; pass < 3; ++pass ) {
		backend.poll();
	}
	EXPECT_NE( -1, listener.indexOf( efsw::Actions::Delete, "entry" ) );
	EXPECT_NE( -1, listener.indexOf( efsw::Actions::Add, "entry" ) );
	EXPECT_EQ( -1, listener.indexOf( efsw::Actions::Moved, "entry" ) );
	listener.clearEvents();
	// Same-directory rename detection is existing behavior, not this option's feature.
	// Check it before changing the contents: metadata-only rename detection is best-effort.
	ASSERT_TRUE( renameFile( destination + "/entry", destination + "/renamed" ) );
	for ( int pass = 0; pass < 3; ++pass ) {
		backend.poll();
	}
	if ( efsw::FileInfo::inodeSupported() ) {
		EXPECT_NE( -1, listener.indexOf( efsw::Actions::Moved, "renamed" ) );
	}
	listener.clearEvents();
	if ( directory && recursive ) {
		ASSERT_TRUE( createFile( destination + "/renamed/after.txt", "x" ) );
	} else {
		ASSERT_TRUE( createFile( destination + "/after.txt", "x" ) );
	}
	for ( int pass = 0; pass < 3; ++pass ) {
		backend.poll();
	}
	EXPECT_NE( -1, listener.indexOf( efsw::Actions::Add, "after.txt" ) );
}

template <typename Backend> void checkRecursive( int* utest_result ) {
	for ( bool enabled : { false, true } ) {
		for ( bool directory : { false, true } ) {
			checkPollingMove<Backend>( false, true, directory, enabled, utest_result );
		}
	}
}

template <typename Backend> void checkIndependent( int* utest_result ) {
	for ( bool enabled : { false, true } ) {
		for ( bool recursive : { false, true } ) {
			for ( bool directory : { false, true } ) {
				checkPollingMove<Backend>( true, recursive, directory, enabled, utest_result );
			}
		}
	}
}

} // namespace

UTEST( PollingMoveOptions, GenericIgnoresFlagWithinRecursiveWatch ) {
	checkRecursive<PollingGeneric>( utest_result );
}

UTEST( PollingMoveOptions, GenericIgnoresFlagBetweenWatches ) {
	checkIndependent<PollingGeneric>( utest_result );
}

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE
UTEST( PollingMoveOptions, KqueueIgnoresFlagWithinRecursiveWatch ) {
	checkRecursive<PollingKqueue>( utest_result );
}

UTEST( PollingMoveOptions, KqueueIgnoresFlagBetweenWatches ) {
	checkIndependent<PollingKqueue>( utest_result );
}

UTEST( PollingMoveOptions, KqueueFallbackIgnoresFlagWithinRecursiveWatch ) {
	checkRecursive<PollingKqueueFallback>( utest_result );
}

UTEST( PollingMoveOptions, KqueueFallbackIgnoresFlagBetweenWatches ) {
	checkIndependent<PollingKqueueFallback>( utest_result );
}
#endif
