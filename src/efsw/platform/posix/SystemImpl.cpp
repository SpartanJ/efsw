#include <efsw/platform/posix/SystemImpl.hpp>

#if defined( EFSW_PLATFORM_POSIX )

#include <pthread.h>
#include <sys/time.h>

namespace efsw { namespace Platform {

void System::sleep( const unsigned long& ms ) {
	// usleep( static_cast<unsigned long>( ms * 1000 ) );

	// usleep is not reliable enough (it might block the
	// whole process instead of just the current thread)
	// so we must use pthread_cond_timedwait instead

	// this implementation is inspired from Qt
	// and taken from SFML

	unsigned long long usecs = ms * 1000;

	// get the current time
	timeval tv;
	gettimeofday(&tv, NULL);

	// construct the time limit (current time + time to wait)
	timespec ti;
	ti.tv_nsec = (tv.tv_usec + (usecs % 1000000)) * 1000;
	ti.tv_sec = tv.tv_sec + (usecs / 1000000) + (ti.tv_nsec / 1000000000);
	ti.tv_nsec %= 1000000000;

	// create a mutex and thread condition
	pthread_mutex_t mutex;
	pthread_mutex_init(&mutex, 0);
	pthread_cond_t condition;
	pthread_cond_init(&condition, 0);

	// wait...
	pthread_mutex_lock(&mutex);
	pthread_cond_timedwait(&condition, &mutex, &ti);
	pthread_mutex_unlock(&mutex);

	// destroy the mutex and condition
	pthread_cond_destroy(&condition);
}

}}

#endif
