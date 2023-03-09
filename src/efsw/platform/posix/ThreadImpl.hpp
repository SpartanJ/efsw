#ifndef EFSW_THREADIMPLPOSIX_HPP
#define EFSW_THREADIMPLPOSIX_HPP

#include <efsw/base.hpp>

#if defined( EFSW_PLATFORM_POSIX )

#include <efsw/Atomic.hpp>
#include <pthread.h>

namespace efsw {

class Thread;

namespace Platform {

class ThreadImpl {
  public:
	ThreadImpl();

	void create( efsw::Thread* owner );

	void wait();

	void terminate();

  protected:
	static void* entryPoint( void* userData );

	pthread_t mThread;
	Atomic<bool> mIsActive;
};

} // namespace Platform
} // namespace efsw

#endif

#endif
