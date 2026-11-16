#include "test_util.hpp"
#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32
#include <efsw/FileInfo.hpp>
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
		}
	}
};

const std::vector<efsw::WatcherOption> crossMoves = {
	{ efsw::Options::ReportCrossDirectoryMoves, 1 } };

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

#endif
