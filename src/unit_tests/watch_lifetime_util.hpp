#ifndef EFSW_WATCH_LIFETIME_UTIL_HPP
#define EFSW_WATCH_LIFETIME_UTIL_HPP

#include "test_util.hpp"
#include <cstdio>
#include <cstdlib>

namespace efsw_test {
// Turn a shutdown deadlock into a test failure instead of hanging the entire CI job.
class ShutdownDeadline {
  public:
	ShutdownDeadline() :
		worker( [this] {
			std::unique_lock<std::mutex> lock( mutex );
			if ( !cv.wait_for( lock, std::chrono::seconds( 10 ), [this] { return done; } ) ) {
				std::fprintf( stderr, "Watcher shutdown timed out\n" );
				std::abort();
			}
		} ) {}

	~ShutdownDeadline() {
		{
			std::lock_guard<std::mutex> lock( mutex );
			done = true;
		}
		cv.notify_one();
		worker.join();
	}

  private:
	std::mutex mutex;
	std::condition_variable cv;
	bool done = false;
	std::thread worker;
};

} // namespace efsw_test

#endif
