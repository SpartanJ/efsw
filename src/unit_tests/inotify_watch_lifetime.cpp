#include "test_util.hpp"
#include "utest.h"
#include "watch_lifetime_util.hpp"
#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY
#include <efsw/FileWatcherInotify.hpp>
#include <efsw/Lock.hpp>

namespace {
class PausedActionWatcher : public efsw::FileWatcherInotify {
  public:
	PausedActionWatcher() : FileWatcherInotify( nullptr ) {
		mThread = new efsw::Thread( [this] {
			while ( mInitOK )
				efsw_test::sleepMs( 1 );
			// Model an action paused after its initial mInitOK check but before mInitLock.
			// Give the destructor time to enter its join before taking the mutex.
			efsw_test::sleepMs( 50 );
			efsw::Lock lock( mInitLock );
		} );
		mThread->launch();
	}
};
} // namespace

UTEST( InotifyWatchLifetime, ShutdownDoesNotHoldActionMutex ) {
	efsw_test::ShutdownDeadline deadline;
	PausedActionWatcher watcher;
}
#endif
