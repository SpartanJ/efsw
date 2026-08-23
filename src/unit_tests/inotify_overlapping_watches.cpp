#include "test_util.hpp"
#include "utest.h"

#include <efsw/base.hpp>
#include <efsw/efsw.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY

// Regression tests for efsw issue #128.
//
// The Linux/inotify backend implements recursive watching with one native
// inotify watch per directory. Two explicit user watches that would both
// require ownership of the same native inotify directory watch are rejected
// with Errors::FileOverlapping before any inotify state is touched, leaving all
// existing watches completely unchanged.

using namespace efsw_test;

namespace {

// Creates root/ and root/sub/, returns false on failure.
bool createTestTree( std::string& root, std::string& sub ) {
	root = getTemporaryDirectory();
	sub = root + "/sub";
	return createDirectory( root ) && createDirectory( sub );
}

} // namespace

UTEST( InotifyOverlapping, RecursiveAncestorThenRecursiveDescendant ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );

	TestListener listenerA;
	TestListener listenerB;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( root, &listenerA, true );
	ASSERT_TRUE( watchA > 0 );

	efsw::WatchID watchB = fileWatcher.addWatch( sub, &listenerB, true );
	EXPECT_EQ( efsw::Errors::FileOverlapping, watchB );

	fileWatcher.watch();
	sleepMs( 300 );

	// The existing recursive tree must be unaffected by the rejected call.
	EXPECT_TRUE( createFile( sub + "/file.txt", "content" ) );
	EXPECT_TRUE( listenerA.waitForActions( efsw::Actions::Add, "file.txt", 3000 ) );

	fileWatcher.removeWatch( watchA );
	removeDirectory( root );
}

UTEST( InotifyOverlapping, RecursiveAncestorThenNonRecursiveDescendant ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );

	TestListener listenerA;
	TestListener listenerB;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( root, &listenerA, true );
	ASSERT_TRUE( watchA > 0 );

	efsw::WatchID watchB = fileWatcher.addWatch( sub, &listenerB, false );
	EXPECT_EQ( efsw::Errors::FileOverlapping, watchB );

	fileWatcher.watch();
	sleepMs( 300 );

	EXPECT_TRUE( createFile( sub + "/file.txt", "content" ) );
	EXPECT_TRUE( listenerA.waitForActions( efsw::Actions::Add, "file.txt", 3000 ) );

	fileWatcher.removeWatch( watchA );
	removeDirectory( root );
}

UTEST( InotifyOverlapping, DescendantThenRecursiveAncestor ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );

	TestListener listenerA;
	TestListener listenerB;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( sub, &listenerA, true );
	ASSERT_TRUE( watchA > 0 );

	efsw::WatchID watchB = fileWatcher.addWatch( root, &listenerB, true );
	EXPECT_EQ( efsw::Errors::FileOverlapping, watchB );

	fileWatcher.watch();
	sleepMs( 300 );

	// Registration order must not change the result, and no partial
	// registration of the rejected recursive ancestor may exist.
	EXPECT_TRUE( createFile( sub + "/file.txt", "content" ) );
	EXPECT_TRUE( listenerA.waitForActions( efsw::Actions::Add, "file.txt", 3000 ) );

	fileWatcher.removeWatch( watchA );
	removeDirectory( root );
}

UTEST( InotifyOverlapping, NonRecursiveDescendantThenRecursiveAncestor ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );

	TestListener listenerA;
	TestListener listenerB;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( sub, &listenerA, false );
	ASSERT_TRUE( watchA > 0 );

	efsw::WatchID watchB = fileWatcher.addWatch( root, &listenerB, true );
	EXPECT_EQ( efsw::Errors::FileOverlapping, watchB );

	fileWatcher.removeWatch( watchA );
	removeDirectory( root );
}

UTEST( InotifyOverlapping, NonRecursiveAncestorWithRecursiveDescendantAllowed ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );

	TestListener listenerA;
	TestListener listenerB;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( root, &listenerA, false );
	ASSERT_TRUE( watchA > 0 );

	efsw::WatchID watchB = fileWatcher.addWatch( sub, &listenerB, true );
	ASSERT_TRUE( watchB > 0 );

	fileWatcher.watch();
	sleepMs( 300 );

	// Both watches must keep working independently.
	EXPECT_TRUE( createFile( root + "/root_file.txt", "content" ) );
	EXPECT_TRUE( createFile( sub + "/sub_file.txt", "content" ) );
	EXPECT_TRUE( listenerA.waitForActions( efsw::Actions::Add, "root_file.txt", 3000 ) );
	EXPECT_TRUE( listenerB.waitForActions( efsw::Actions::Add, "sub_file.txt", 3000 ) );

	fileWatcher.removeWatch( watchA );
	fileWatcher.removeWatch( watchB );
	removeDirectory( root );
}

UTEST( InotifyOverlapping, NestedNonRecursiveWatchesAllowed ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );

	TestListener listenerA;
	TestListener listenerB;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( root, &listenerA, false );
	ASSERT_TRUE( watchA > 0 );

	efsw::WatchID watchB = fileWatcher.addWatch( sub, &listenerB, false );
	ASSERT_TRUE( watchB > 0 );

	fileWatcher.removeWatch( watchA );
	fileWatcher.removeWatch( watchB );
	removeDirectory( root );
}

UTEST( InotifyOverlapping, SiblingRecursiveWatchesAllowed ) {
	if ( useGeneric )
		return;

	std::string root = getTemporaryDirectory();
	std::string dirA = root + "/a";
	std::string dirB = root + "/b";
	EXPECT_TRUE( createDirectory( root ) );
	EXPECT_TRUE( createDirectory( dirA ) );
	EXPECT_TRUE( createDirectory( dirB ) );

	TestListener listenerA;
	TestListener listenerB;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( dirA, &listenerA, true );
	ASSERT_TRUE( watchA > 0 );

	efsw::WatchID watchB = fileWatcher.addWatch( dirB, &listenerB, true );
	ASSERT_TRUE( watchB > 0 );

	fileWatcher.removeWatch( watchA );
	fileWatcher.removeWatch( watchB );
	removeDirectory( root );
}

UTEST( InotifyOverlapping, ExactDuplicateRemainsRejected ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );

	TestListener listenerA;
	TestListener listenerB;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( root, &listenerA, false );
	ASSERT_TRUE( watchA > 0 );

	efsw::WatchID watchB = fileWatcher.addWatch( root, &listenerB, false );
	EXPECT_EQ( efsw::Errors::FileRepeated, watchB );

	fileWatcher.removeWatch( watchA );
	removeDirectory( root );
}

UTEST( InotifyOverlapping, CanonicalPathAliasIsRejected ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );

	TestListener listenerA;
	TestListener listenerB;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( root, &listenerA, true );
	ASSERT_TRUE( watchA > 0 );

	const std::filesystem::path rootPath( root );
	const std::string aliasedSub =
		( rootPath / ".." / rootPath.filename() / "sub" ).string();
	efsw::WatchID watchB = fileWatcher.addWatch( aliasedSub, &listenerB, true );
	EXPECT_EQ( efsw::Errors::FileOverlapping, watchB );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( sub + "/alias_file.txt", "content" ) );
	EXPECT_TRUE( listenerA.waitForActions( efsw::Actions::Add, "alias_file.txt", 3000 ) );

	fileWatcher.removeWatch( watchA );
	removeDirectory( root );
}

UTEST( InotifyOverlapping, SymlinkOutsideLexicalTreeIsAllowed ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );
	const std::string outside = root + "_outside";
	const std::string link = root + "/outside_link";
	ASSERT_TRUE( createDirectory( outside ) );

	std::error_code error;
	std::filesystem::create_directory_symlink( outside, link, error );
	ASSERT_FALSE( static_cast<bool>( error ) );

	TestListener listenerA;
	TestListener listenerB;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	// Register the recursive root while symlink traversal is disabled, so its
	// native tree deliberately does not include the external target.
	efsw::WatchID watchA = fileWatcher.addWatch( root, &listenerA, true );
	ASSERT_TRUE( watchA > 0 );

	fileWatcher.followSymlinks( true );
	fileWatcher.allowOutOfScopeLinks( true );
	efsw::WatchID watchB = fileWatcher.addWatch( link, &listenerB, true );
	ASSERT_TRUE( watchB > 0 );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( outside + "/outside_file.txt", "content" ) );
	EXPECT_TRUE( listenerB.waitForActions( efsw::Actions::Add, "outside_file.txt", 3000 ) );

	fileWatcher.removeWatch( watchA );
	fileWatcher.removeWatch( watchB );
	removeDirectory( root );
	removeDirectory( outside );
}

UTEST( InotifyOverlapping, SymlinkTargetInsideRecursiveTreeIsRejected ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );
	const std::string links = root + "_links";
	const std::string link = links + "/sub_link";
	ASSERT_TRUE( createDirectory( links ) );

	std::error_code error;
	std::filesystem::create_directory_symlink( sub, link, error );
	ASSERT_FALSE( static_cast<bool>( error ) );

	TestListener listenerA;
	TestListener listenerB;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( root, &listenerA, true );
	ASSERT_TRUE( watchA > 0 );

	fileWatcher.followSymlinks( true );
	fileWatcher.allowOutOfScopeLinks( true );
	efsw::WatchID watchB = fileWatcher.addWatch( link, &listenerB, true );
	EXPECT_EQ( efsw::Errors::FileOverlapping, watchB );

	fileWatcher.watch();
	EXPECT_TRUE( createFile( sub + "/symlink_file.txt", "content" ) );
	EXPECT_TRUE( listenerA.waitForActions( efsw::Actions::Add, "symlink_file.txt", 3000 ) );

	fileWatcher.removeWatch( watchA );
	removeDirectory( root );
	removeDirectory( links );
}

UTEST( InotifyOverlapping, RejectedWatchLeavesCleanState ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );

	TestListener listenerA;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( root, &listenerA, true );
	ASSERT_TRUE( watchA > 0 );

	efsw::WatchID watchB = fileWatcher.addWatch( sub, &listenerA, true );
	EXPECT_EQ( efsw::Errors::FileOverlapping, watchB );

	// Removing the original watch and re-adding its former subtree must work:
	// the rejected attempt introduced no stale internal or native state.
	fileWatcher.removeWatch( watchA );

	efsw::WatchID watchC = fileWatcher.addWatch( sub, &listenerA, true );
	ASSERT_TRUE( watchC > 0 );

	fileWatcher.removeWatch( watchC );
	removeDirectory( root );
}

UTEST( InotifyOverlapping, RejectedRecursiveAncestorLeavesCleanState ) {
	if ( useGeneric )
		return;

	std::string root;
	std::string sub;
	ASSERT_TRUE( createTestTree( root, sub ) );

	TestListener listenerA;
	efsw::FileWatcher fileWatcher( useGeneric, 100 );

	efsw::WatchID watchA = fileWatcher.addWatch( sub, &listenerA, true );
	ASSERT_TRUE( watchA > 0 );

	efsw::WatchID watchB = fileWatcher.addWatch( root, &listenerA, true );
	EXPECT_EQ( efsw::Errors::FileOverlapping, watchB );

	fileWatcher.removeWatch( watchA );

	// No partial setup of the rejected recursive ancestor may remain.
	efsw::WatchID watchC = fileWatcher.addWatch( root, &listenerA, true );
	ASSERT_TRUE( watchC > 0 );

	fileWatcher.removeWatch( watchC );
	removeDirectory( root );
}

#endif // EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY
