#include "test_util.hpp"
#include "utest.h"
#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_KQUEUE

#include <efsw/FileWatcherKqueue.hpp>
#include <efsw/WatcherKqueue.hpp>
#include <fcntl.h>

using namespace efsw_test;

namespace {

struct ReopeningWatch : efsw::WatcherKqueue {

	using efsw::WatcherKqueue::WatcherKqueue;

	void snapshotDirectory() { mDirSnap.DirectoryInfo = efsw::FileInfo( Directory ); }

	int directoryFd() const { return static_cast<int>( mChangeList[0].ident ); }

	int changeQueue( int queue ) {
		const int previous = mKqueue;
		mKqueue = queue;
		return previous;
	}
};

void checkReopenFailure( bool registrationFails, int* utest_result ) {
	const std::string root = getTemporaryDirectory() + "_kq_reopen";
	const std::string watched = root + "/watched";
	ASSERT_TRUE( createDirectory( root ) );
	ASSERT_TRUE( createDirectory( watched ) );
	ASSERT_TRUE( createDirectory( root + "/replacement" ) );
	TestListener listener;
	{
		efsw::FileWatcher owner;
		efsw::FileWatcherKqueue backend( &owner, 100 );
		ReopeningWatch watch( 1, watched, &listener, true, &backend );
		watch.addAll();
		ASSERT_TRUE( watch.initOK() );
		const int original = watch.directoryFd();
		ASSERT_TRUE( renameFile( watched, root + "/original" ) );
		// Consume the old descriptor's rename notification while its path is absent. The
		// retry must work without another native event waking it afterwards.
		watch.watch();
		ASSERT_TRUE( renameFile( root + "/replacement", watched ) );
		// The path snapshot advances before reopening, just as rescan() does. A failure must
		// not make that identity look like the one the native descriptor watches.
		watch.snapshotDirectory();
		if ( registrationFails ) {
			const int queue = watch.changeQueue( -1 );
			EXPECT_FALSE( watch.reopenDirectory() );
			watch.changeQueue( queue );
		} else {
			ASSERT_TRUE( renameFile( watched, root + "/temporarily_away" ) );
			EXPECT_FALSE( watch.reopenDirectory() );
			ASSERT_TRUE( renameFile( root + "/temporarily_away", watched ) );
		}
		EXPECT_EQ( original, watch.directoryFd() );
		EXPECT_NE( -1, fcntl( original, F_GETFD ) );
		watch.watch();
		listener.clearEvents();
		ASSERT_TRUE( createFile( watched + "/after_retry", "x" ) );
		watch.watch();
		EXPECT_NE( -1, listener.indexOf( efsw::Actions::Add, "after_retry" ) );
	}
	removeDirectory( root );
}

} // namespace

UTEST( KqueueReopen, RetriesAnOpenFailureAfterTheSnapshotAdvanced ) {
	checkReopenFailure( false, utest_result );
}

UTEST( KqueueReopen, FailedRegistrationKeepsTheOldDescriptorAndRetries ) {
	checkReopenFailure( true, utest_result );
}

UTEST( KqueueReopen, ReplacementFilesAtTheSameNamesKeepReportingWrites ) {
	const std::string root = getTemporaryDirectory() + "_kq_same_name";
	const std::string watched = root + "/watched";
	ASSERT_TRUE( createDirectory( root ) );
	ASSERT_TRUE( createDirectory( watched ) );
	ASSERT_TRUE( createDirectory( root + "/replacement" ) );
	ASSERT_TRUE( createFile( watched + "/existing", "old" ) );
	ASSERT_TRUE( createFile( root + "/replacement/existing", "new" ) );
	TestListener listener;
	{
		efsw::FileWatcher owner;
		efsw::FileWatcherKqueue backend( &owner, 100 );
		ReopeningWatch watch( 1, watched, &listener, true, &backend );
		watch.addAll();
		ASSERT_TRUE( watch.initOK() );
		ASSERT_TRUE( renameFile( watched, root + "/original" ) );
		ASSERT_TRUE( renameFile( root + "/replacement", watched ) );
		watch.watch();
		listener.clearEvents();
		ASSERT_TRUE( writeFile( watched + "/existing", " written after replacement" ) );
		watch.watch();
		EXPECT_NE( -1, listener.indexOf( efsw::Actions::Modified, "existing" ) );
	}
	removeDirectory( root );
}

#endif
