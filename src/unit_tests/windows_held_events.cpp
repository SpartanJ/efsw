#include "test_util.hpp"
#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32
#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/FileWatcherWin32.hpp>
#include <efsw/Lock.hpp>
#include <efsw/WatcherWin32.hpp>
#include <string>
#include <vector>

// utest must see the Windows headers before declaring its timer functions.
#include "utest.h"

using namespace efsw_test;

namespace {

struct Record {
	DWORD Action;
	std::string FileName;
	LONGLONG FileId;
};

/// A Windows watcher whose watches are given extended notification records directly, without
/// its thread: the records a ReadDirectoryChangesExW buffer holds, in order.
class InjectingWatcher : public efsw::FileWatcherWin32 {
  public:
	InjectingWatcher() : FileWatcherWin32( nullptr ) {}

	void inject( efsw::WatchID id, const std::vector<Record>& records ) {
		efsw::Lock lock( mWatchesLock );
		for ( auto watch : mWatches ) {
			if ( watch->Watch->ID != id )
				continue;
			watch->Watch->ExtendedEvents.clear();
			for ( const Record& record : records ) {
				efsw::ExtendedEventWin32 event;
				event.Action = record.Action;
				event.FileName = record.FileName;
				event.FileId.QuadPart = record.FileId;
				watch->Watch->ExtendedEvents.push_back( event );
			}
			efsw::ProcessExtendedEvents( watch->Watch );
			// Processing can remove this registration and invalidate the set iterator.
			return;
		}
	}
};

const std::vector<efsw::WatcherOption> crossMoves = {
	{ efsw::Options::ReportCrossDirectoryMoves, 1 } };

enum class RemovedRegistration { Source, Destination, Both };

struct RemovingListener : TestListener {
	InjectingWatcher* Watcher{ nullptr };
	efsw::WatchID First{ 0 };
	efsw::WatchID Second{ 0 };

	void handleFileAction( efsw::WatchID id, const std::string& directory,
						   const std::string& filename, efsw::Action action,
						   const std::string& oldFilename ) override {
		TestListener::handleFileAction( id, directory, filename, action, oldFilename );
		if ( First ) {
			const efsw::WatchID first = First;
			First = 0;
			Watcher->removeWatch( first );
			if ( Second ) {
				Watcher->removeWatch( Second );
			}
		}
	}
};

// The source half is consumed before either flush calls out. Canceling the destination from
// an older callback must leave the source's Delete intact; canceling the source alone must
// still allow the destination's Moved. Drive both arrival orders without timing or a worker.
void checkRemovalDuringMoveFlush( bool destinationFirst, bool flushDestination,
								 RemovedRegistration removed, int* utest_result ) {
	const std::string root = getTemporaryDirectory() + "_held_cancel";
	struct Cleanup {
		const std::string& Root;
		~Cleanup() { removeDirectory( Root ); }
	} cleanup{ root };
	ASSERT_TRUE( createDirectory( root ) );
	const std::string source = root + "/source";
	const std::string destination = root + "/destination";
	ASSERT_TRUE( createDirectory( source ) );
	ASSERT_TRUE( createDirectory( destination ) );
	// The scripted records model an already completed move of this recursive tree.
	ASSERT_TRUE( createDirectory( destination + "/d" ) );
	ASSERT_TRUE( createDirectory( destination + "/d/sub" ) );
	ASSERT_TRUE( createFile( destination + "/d/sub/kept.txt", "x" ) );
	const efsw::FileInfo moved( destination + "/d" );
	ASSERT_NE( 0ull, static_cast<unsigned long long>( moved.Inode ) );
	const LONGLONG id = static_cast<LONGLONG>( moved.Inode );
	const LONGLONG olderId = id == 1 ? 2 : 1;
	RemovingListener sourceListener;
	RemovingListener destinationListener;
	InjectingWatcher watcher;
	const efsw::WatchID from = watcher.addWatch( source, &sourceListener, true, crossMoves );
	const efsw::WatchID to = watcher.addWatch( destination, &destinationListener, true, crossMoves );
	ASSERT_GT( from, 0 );
	ASSERT_GT( to, 0 );
	RemovingListener& trigger = flushDestination ? destinationListener : sourceListener;
	trigger.Watcher = &watcher;
	trigger.First = removed == RemovedRegistration::Source ? from : to;
	trigger.Second = removed == RemovedRegistration::Both ? from : 0;
	// At the destination the older Delete is a previous occupant of the arrival's path.
	watcher.inject( flushDestination ? to : from,
					{ { FILE_ACTION_REMOVED, flushDestination ? "d" : "d\\child.txt", olderId } } );
	if ( destinationFirst ) {
		watcher.inject( to, { { FILE_ACTION_ADDED, "d", id } } );
		EXPECT_EQ( 0u, destinationListener.getEventCount() );
		watcher.inject( from, { { FILE_ACTION_REMOVED, "d", id } } );
	} else {
		watcher.inject( from, { { FILE_ACTION_REMOVED, "d", id } } );
		EXPECT_EQ( 0u, sourceListener.getEventCount() );
		watcher.inject( to, { { FILE_ACTION_ADDED, "d", id } } );
	}

	const auto sourceEvents = sourceListener.getEvents();
	const auto destinationEvents = destinationListener.getEvents();
	const size_t flushedSource = flushDestination ? 0u : 1u;
	const size_t flushedDestination = flushDestination ? 1u : 0u;
	if ( removed == RemovedRegistration::Destination ) {
		ASSERT_EQ( flushedSource + 1u, sourceEvents.size() );
		EXPECT_EQ( efsw::Actions::Delete, std::get<0>( sourceEvents.back() ) );
		EXPECT_TRUE( std::get<1>( sourceEvents.back() ) == "d" );
		EXPECT_EQ( flushedDestination, destinationEvents.size() );
	} else if ( removed == RemovedRegistration::Source ) {
		EXPECT_EQ( flushedSource, sourceEvents.size() );
		ASSERT_EQ( flushedDestination + 1u, destinationEvents.size() );
		EXPECT_EQ( efsw::Actions::Moved, std::get<0>( destinationEvents.back() ) );
		EXPECT_TRUE( std::get<1>( destinationEvents.back() ) == "d" );
		EXPECT_TRUE( std::get<2>( destinationEvents.back() ) ==
					 efsw::FileSystem::canonicalSourcePath( source + "\\d" ) );
	} else {
		EXPECT_EQ( flushedSource, sourceEvents.size() );
		EXPECT_EQ( flushedDestination, destinationEvents.size() );
	}
	const auto flushed = trigger.getEvents();
	ASSERT_FALSE( flushed.empty() );
	EXPECT_EQ( efsw::Actions::Delete, std::get<0>( flushed[0] ) );
	EXPECT_TRUE( std::get<1>( flushed[0] ) == ( flushDestination ? "d" : "child.txt" ) );
}

} // namespace

// A directory and a file inside it created, then the directory renamed, before the Adds' wait to
// pair them ended: the file's Add comes before the rename, which takes its path away.
UTEST( WindowsHeldEvents, RenamedDirectoryReportsHeldContentsFirst ) {
	const std::string dir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( dir ) );
	TestListener listener;
	{
		InjectingWatcher watcher;
		const efsw::WatchID id = watcher.addWatch( dir, &listener, true, crossMoves );
		ASSERT_GT( id, 0 );
		watcher.inject( id, { { FILE_ACTION_ADDED, "d", 10 },
							  { FILE_ACTION_ADDED, "d\\child.txt", 11 },
							  { FILE_ACTION_RENAMED_OLD_NAME, "d", 10 },
							  { FILE_ACTION_RENAMED_NEW_NAME, "e", 10 } } );

		auto events = listener.getEvents();
		ASSERT_EQ( 3u, events.size() );
		EXPECT_EQ( efsw::Actions::Add, std::get<0>( events[0] ) );
		EXPECT_TRUE( std::get<1>( events[0] ) == "d" );
		EXPECT_EQ( efsw::Actions::Add, std::get<0>( events[1] ) );
		EXPECT_TRUE( std::get<1>( events[1] ) == "child.txt" );
		EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[2] ) );
		EXPECT_TRUE( std::get<1>( events[2] ) == "e" );
	}
	removeDirectory( dir );
}

// A directory removed without a file identity, which is reported at once: the Add held inside it
// comes first.
UTEST( WindowsHeldEvents, RemovedDirectoryReportsHeldContentsFirst ) {
	const std::string dir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( dir ) );
	TestListener listener;
	{
		InjectingWatcher watcher;
		const efsw::WatchID id = watcher.addWatch( dir, &listener, true, crossMoves );
		ASSERT_GT( id, 0 );
		watcher.inject(
			id, { { FILE_ACTION_ADDED, "d\\child.txt", 11 }, { FILE_ACTION_REMOVED, "d", 0 } } );

		auto events = listener.getEvents();
		ASSERT_EQ( 2u, events.size() );
		EXPECT_EQ( efsw::Actions::Add, std::get<0>( events[0] ) );
		EXPECT_TRUE( std::get<1>( events[0] ) == "child.txt" );
		EXPECT_EQ( efsw::Actions::Delete, std::get<0>( events[1] ) );
		EXPECT_TRUE( std::get<1>( events[1] ) == "d" );
	}
	removeDirectory( dir );
}

// A directory and a file inside it removed with file identities, so both are held, then another
// directory renamed to its name: the file's Delete comes before the directory's, and both before
// the rename, so it cannot be taken for a file inside the renamed directory.
UTEST( WindowsHeldEvents, HeldRemovedDirectoryReportsHeldContentsFirst ) {
	const std::string dir = getTemporaryDirectory();
	ASSERT_TRUE( createDirectory( dir ) );
	TestListener listener;
	{
		InjectingWatcher watcher;
		const efsw::WatchID id = watcher.addWatch( dir, &listener, true, crossMoves );
		ASSERT_GT( id, 0 );
		watcher.inject( id, { { FILE_ACTION_REMOVED, "d\\child.txt", 11 },
							  { FILE_ACTION_REMOVED, "d", 10 },
							  { FILE_ACTION_RENAMED_OLD_NAME, "a", 20 },
							  { FILE_ACTION_RENAMED_NEW_NAME, "d", 20 } } );

		auto events = listener.getEvents();
		ASSERT_EQ( 3u, events.size() );
		EXPECT_EQ( efsw::Actions::Delete, std::get<0>( events[0] ) );
		EXPECT_TRUE( std::get<1>( events[0] ) == "child.txt" );
		EXPECT_EQ( efsw::Actions::Delete, std::get<0>( events[1] ) );
		EXPECT_TRUE( std::get<1>( events[1] ) == "d" );
		EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[2] ) );
		EXPECT_TRUE( std::get<1>( events[2] ) == "d" );
		EXPECT_TRUE( std::get<2>( events[2] ) == "a" );
	}
	removeDirectory( dir );
}

// A directory moved to another registration while an Add inside it is held: the Add comes before
// the Moved event, which takes its path away.
UTEST( WindowsHeldEvents, DirectoryMovedToAnotherWatchReportsHeldContentsFirst ) {
	const std::string source = getTemporaryDirectory() + "_held_source";
	const std::string destination = getTemporaryDirectory() + "_held_destination";
	removeDirectory( source );
	removeDirectory( destination );
	ASSERT_TRUE( createDirectory( source ) );
	ASSERT_TRUE( createDirectory( destination ) );
	// The matcher checks that the destination is the moved directory: create it there.
	ASSERT_TRUE( createDirectory( destination + "\\d" ) );
	const efsw::FileInfo moved( destination + "\\d" );
	ASSERT_NE( 0ull, static_cast<unsigned long long>( moved.Inode ) );
	TestListener listener;
	{
		InjectingWatcher watcher;
		const efsw::WatchID from = watcher.addWatch( source, &listener, true, crossMoves );
		const efsw::WatchID to = watcher.addWatch( destination, &listener, true, crossMoves );
		ASSERT_GT( from, 0 );
		ASSERT_GT( to, 0 );
		const LONGLONG id = static_cast<LONGLONG>( moved.Inode );
		watcher.inject(
			from, { { FILE_ACTION_ADDED, "d\\child.txt", 11 }, { FILE_ACTION_REMOVED, "d", id } } );
		EXPECT_EQ( 0u, listener.getEventCount() );
		watcher.inject( to, { { FILE_ACTION_ADDED, "d", id } } );

		auto events = listener.getEvents();
		ASSERT_EQ( 2u, events.size() );
		EXPECT_EQ( efsw::Actions::Add, std::get<0>( events[0] ) );
		EXPECT_TRUE( std::get<1>( events[0] ) == "child.txt" );
		EXPECT_EQ( efsw::Actions::Moved, std::get<0>( events[1] ) );
		EXPECT_TRUE( std::get<1>( events[1] ) == "d" );
	}
	removeDirectory( source );
	removeDirectory( destination );
}

UTEST( WindowsHeldEvents, DestinationRemovedDuringSourceFlushKeepsSourceDelete ) {
	for ( bool destinationFirst : { false, true } ) {
		checkRemovalDuringMoveFlush( destinationFirst, false, RemovedRegistration::Destination,
									utest_result );
	}
}

UTEST( WindowsHeldEvents, DestinationRemovedDuringDestinationFlushKeepsSourceDelete ) {
	for ( bool destinationFirst : { false, true } ) {
		checkRemovalDuringMoveFlush( destinationFirst, true, RemovedRegistration::Destination,
									utest_result );
	}
}

UTEST( WindowsHeldEvents, SourceRemovedDuringFlushStillDeliversMove ) {
	for ( bool destinationFirst : { false, true } ) {
		for ( bool flushDestination : { false, true } ) {
			checkRemovalDuringMoveFlush( destinationFirst, flushDestination,
										RemovedRegistration::Source, utest_result );
		}
	}
}

UTEST( WindowsHeldEvents, BothRemovedDuringFlushStopDelivery ) {
	for ( bool destinationFirst : { false, true } ) {
		for ( bool flushDestination : { false, true } ) {
			checkRemovalDuringMoveFlush( destinationFirst, flushDestination,
										RemovedRegistration::Both, utest_result );
		}
	}
}

#endif
