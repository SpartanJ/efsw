#ifndef EFSW_INOTIFYBATCH_HPP
#define EFSW_INOTIFYBATCH_HPP

#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/inotify.h>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace efsw {

/// One batch of inotify events, in kernel queue order, with the descriptor of each move's
/// destination.
///
/// inotify reports a rename as IN_MOVED_FROM and IN_MOVED_TO with one cookie. The kernel queues
/// them one after the other, and unrelated events may come between them, so a read may take the
/// source before its destination is queued. While a source in the batch has no destination, the
/// next reads are appended to the batch, which keeps the kernel's order, for at most
/// MoveDestinationWaitMs from the end of the first read: a source moved out of the watched
/// directories has no destination, and a steady stream of them must not hold the batch back.
class InotifyBatch {
  public:
	struct Event {
		int wd;
		uint32_t mask;
		uint32_t cookie;
		std::string filename;
	};

	static const int MoveDestinationWaitMs = 20;

	std::vector<Event> Events;
	/// The descriptor of each move's destination, by cookie.
	std::unordered_map<uint32_t, int> Destinations;

	/// Reads a batch: one read, and while a source in it has no destination, the reads that
	/// follow within the time allowed.
	/// @param source Gives the events: wait( timeoutMs ) is true once a read is ready,
	/// read( buffer, size ) is read(2), nowMs() is a monotonic clock in milliseconds, and
	/// running() is false once the watcher shuts down.
	/// @return False when the first read failed.
	template <typename Source> bool read( Source& source, char* buffer, size_t size ) {
		Events.clear();
		Destinations.clear();
		mUnpaired.clear();
		const ssize_t length = source.read( buffer, size );
		if ( length <= 0 )
			return false;
		// Parsing counts toward the time allowed.
		const long long deadline = source.nowMs() + MoveDestinationWaitMs;
		parse( buffer, length );
		while ( !mUnpaired.empty() && source.running() ) {
			const long long remaining = deadline - source.nowMs();
			if ( remaining <= 0 || !source.wait( static_cast<int>( remaining ) ) )
				break;
			const ssize_t more = source.read( buffer, size );
			if ( more <= 0 )
				break;
			parse( buffer, more );
		}
		return true;
	}

  private:
	/// The cookies of the sources in the batch without a destination yet.
	std::unordered_set<uint32_t> mUnpaired;

	void parse( const char* buffer, ssize_t length ) {
		ssize_t i = 0;
		while ( i < length ) {
			const struct inotify_event* event =
				reinterpret_cast<const struct inotify_event*>( buffer + i );
			Events.push_back( Event{ event->wd, event->mask, event->cookie,
									 event->len ? std::string( event->name ) : std::string() } );
			if ( event->mask & IN_MOVED_FROM ) {
				if ( !Destinations.count( event->cookie ) )
					mUnpaired.insert( event->cookie );
			} else if ( event->mask & IN_MOVED_TO ) {
				Destinations[event->cookie] = event->wd;
				mUnpaired.erase( event->cookie );
			}
			i += sizeof( struct inotify_event ) + event->len;
		}
	}
};

} // namespace efsw

#endif

#endif
