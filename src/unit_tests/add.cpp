#include "test_util.hpp"
#include "utest.h"

#include <map>

using namespace efsw_test;

UTEST( Add, SingleFile ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	std::string testFile = testDir + "/test_file.txt";
	EXPECT_TRUE( createFile( testFile, "test content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "test_file.txt", 3000 ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Add, SingleFileEmoji ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	std::filesystem::path fileName = u8"test_file🐈.txt";

	std::filesystem::path testFile = testDir / fileName;
	EXPECT_TRUE( createFileFs( testFile, "test content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, fileName.u8string(), 3000 ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Add, MultipleFiles ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	EXPECT_TRUE( createFile( testDir + "/file1.txt", "content1" ) );
	EXPECT_TRUE( createFile( testDir + "/file2.txt", "content2" ) );
	EXPECT_TRUE( createFile( testDir + "/file3.txt", "content3" ) );

	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file1.txt", 3000 ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file2.txt", 3000 ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "file3.txt", 3000 ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

UTEST( Add, Subdirectory ) {
	std::string testDir = getTemporaryDirectory();
	std::string subDir = testDir + "/subdir";

	EXPECT_TRUE( createDirectory( testDir ) );
	EXPECT_TRUE( createDirectory( subDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, true );
	EXPECT_TRUE( watchId > 0 );

	fileWatcher.watch();

	std::string testFile = subDir + "/nested_file.txt";
	EXPECT_TRUE( createFile( testFile, "nested content" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "nested_file.txt", 3000 ) );

	fileWatcher.removeWatch( testDir );
	removeDirectory( testDir );
}

// A non-recursive watch reports a subdirectory created after watching started as one of its
// entries, but does not report what happens inside it, and does not stop it from being
// registered on its own.
UTEST( Add, NonRecursiveIgnoresNewSubdirectoryContents ) {
	std::string testDir = getTemporaryDirectory();
	EXPECT_TRUE( createDirectory( testDir ) );

	TestListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );
	efsw::WatchID watchId = fileWatcher.addWatch( testDir, &listener, false );
	EXPECT_TRUE( watchId > 0 );
	fileWatcher.watch();

	EXPECT_TRUE( createDirectory( testDir + "/sub" ) );
	EXPECT_TRUE( listener.waitForActions( efsw::Actions::Add, "sub", 3000 ) );
	EXPECT_TRUE( createFile( testDir + "/sub/inside.txt", "x" ) );
	EXPECT_TRUE( waitForPending( listener, testDir ) );
	EXPECT_EQ( -1, listener.indexOf( efsw::Actions::Add, "inside.txt" ) );

	TestListener subListener;
	efsw::WatchID subId = fileWatcher.addWatch( testDir + "/sub", &subListener, false );
	EXPECT_TRUE( subId > 0 );

	if ( subId > 0 )
		fileWatcher.removeWatch( subId );
	fileWatcher.removeWatch( watchId );
	removeDirectory( testDir );
}

namespace {

/// Records the watch ID of each Add, by file name.
class IdListener : public efsw::FileWatchListener {
  public:
	void handleFileAction( efsw::WatchID id, const std::string&, const std::string& filename,
						   efsw::Action action, const std::string& ) override {
		if ( action != efsw::Actions::Add )
			return;
		std::lock_guard<std::mutex> lock( mLock );
		mIds[filename] = id;
		mChanged.notify_all();
	}

	/// The ID the Add of the file came with, or 0 if none came within three seconds.
	efsw::WatchID idOf( const std::string& filename ) {
		std::unique_lock<std::mutex> lock( mLock );
		mChanged.wait_for( lock, std::chrono::seconds( 3 ),
						   [&] { return mIds.count( filename ) > 0; } );
		auto found = mIds.find( filename );
		return found != mIds.end() ? found->second : 0;
	}

  private:
	std::mutex mLock;
	std::condition_variable mChanged;
	std::map<std::string, efsw::WatchID> mIds;
};

} // namespace

// An event deep inside a recursive registration comes with the ID addWatch() returned for it,
// whichever directory of it reported the event: a listener tells its registrations apart by it.
UTEST( Add, EventsInSubdirectoriesHaveTheRegistrationId ) {
	const std::string root = getTemporaryDirectory();
	const std::string first = root + "/first";
	const std::string second = root + "/second";
	EXPECT_TRUE( createDirectory( root ) );
	for ( const std::string& directory :
		  { first, first + "/a", first + "/a/b", second, second + "/c", second + "/c/d" } )
		EXPECT_TRUE( createDirectory( directory ) );

	IdListener listener;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );
	const efsw::WatchID firstId = fileWatcher.addWatch( first, &listener, true );
	const efsw::WatchID secondId = fileWatcher.addWatch( second, &listener, true );
	EXPECT_TRUE( firstId > 0 );
	EXPECT_TRUE( secondId > 0 );
	fileWatcher.watch();

	EXPECT_TRUE( createFile( first + "/a/b/in_first.txt", "x" ) );
	EXPECT_TRUE( createFile( second + "/c/d/in_second.txt", "x" ) );
	EXPECT_TRUE( createFile( second + "/c/in_second_top.txt", "x" ) );
	EXPECT_EQ( firstId, listener.idOf( "in_first.txt" ) );
	EXPECT_EQ( secondId, listener.idOf( "in_second.txt" ) );
	EXPECT_EQ( secondId, listener.idOf( "in_second_top.txt" ) );

	fileWatcher.removeWatch( first );
	fileWatcher.removeWatch( second );
	removeDirectory( root );
}
