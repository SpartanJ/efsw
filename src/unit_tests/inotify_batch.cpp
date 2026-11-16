#include "test_util.hpp"
#include "utest.h"

#include <efsw/base.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_INOTIFY

#include <cstring>
#include <efsw/InotifyBatch.hpp>
#include <string>
#include <vector>

namespace {

/// Reads scripted for an InotifyBatch, each ready at a time on a virtual clock that waiting
/// advances.
class ScriptedSource {
  public:
	struct Read {
		long long At;
		std::vector<char> Bytes;
	};

	std::vector<Read> Reads;
	long long Now{ 0 };
	size_t Next{ 0 };
	bool Running{ true };
	int Waits{ 0 };
	/// The number of waits after which the watcher shuts down, if any.
	int StopAfterWaits{ -1 };

	/// Adds a read, ready at a time, of events added with event().
	void readAt( long long at ) { Reads.push_back( { at, {} } ); }

	/// Adds an event to the last read.
	void event( int wd, uint32_t mask, uint32_t cookie, const std::string& name ) {
		struct inotify_event header;
		std::memset( &header, 0, sizeof( header ) );
		header.wd = wd;
		header.mask = mask;
		header.cookie = cookie;
		// The kernel pads names with NUL bytes to an aligned length.
		header.len = name.empty() ? 0 : static_cast<uint32_t>( ( name.size() + 16 ) / 16 * 16 );
		std::vector<char>& bytes = Reads.back().Bytes;
		const char* begin = reinterpret_cast<const char*>( &header );
		bytes.insert( bytes.end(), begin, begin + sizeof( header ) );
		std::vector<char> padded( header.len, '\0' );
		if ( !name.empty() )
			std::memcpy( padded.data(), name.data(), name.size() );
		bytes.insert( bytes.end(), padded.begin(), padded.end() );
	}

	bool wait( int timeoutMs ) {
		if ( ++Waits == StopAfterWaits )
			Running = false;
		if ( Next < Reads.size() && Reads[Next].At <= Now + timeoutMs ) {
			if ( Reads[Next].At > Now )
				Now = Reads[Next].At;
			return true;
		}
		Now += timeoutMs;
		return false;
	}

	ssize_t read( char* buffer, size_t size ) {
		if ( Next == Reads.size() || Reads[Next].Bytes.size() > size )
			return -1;
		const std::vector<char>& bytes = Reads[Next++].Bytes;
		std::memcpy( buffer, bytes.data(), bytes.size() );
		return static_cast<ssize_t>( bytes.size() );
	}

	long long nowMs() const { return Now; }

	bool running() const { return Running; }
};

/// Reads a batch from the source at its current time.
bool readBatch( efsw::InotifyBatch& batch, ScriptedSource& source ) {
	std::vector<char> buffer( 4096 );
	return batch.read( source, buffer.data(), buffer.size() );
}

} // namespace

// A rename whose destination is queued after the read that took its source: the next read is
// part of the batch, which pairs them.
UTEST( InotifyBatch, DestinationInTheNextRead ) {
	ScriptedSource source;
	source.readAt( 0 );
	source.event( 1, IN_MOVED_FROM, 7, "old" );
	source.readAt( 1 );
	source.event( 2, IN_MOVED_TO, 7, "new" );
	efsw::InotifyBatch batch;
	ASSERT_TRUE( readBatch( batch, source ) );
	const auto& events = batch.Events;

	ASSERT_EQ( 2u, events.size() );
	EXPECT_TRUE( events[0].filename == "old" );
	EXPECT_TRUE( events[1].filename == "new" );
	ASSERT_EQ( 1u, batch.Destinations.count( 7 ) );
	EXPECT_EQ( 2, batch.Destinations[7] );
	EXPECT_EQ( 1, source.Now );
}

// An unrelated event between the source and its destination, in a read of its own: the batch
// still waits for the destination, and keeps the kernel's order.
UTEST( InotifyBatch, UnrelatedEventBetweenSourceAndDestination ) {
	ScriptedSource source;
	source.readAt( 0 );
	source.event( 1, IN_MOVED_FROM, 7, "old" );
	source.readAt( 1 );
	source.event( 1, IN_CREATE, 0, "other" );
	source.readAt( 2 );
	source.event( 2, IN_MOVED_TO, 7, "new" );
	efsw::InotifyBatch batch;
	ASSERT_TRUE( readBatch( batch, source ) );
	const auto& events = batch.Events;

	ASSERT_EQ( 3u, events.size() );
	EXPECT_TRUE( events[0].filename == "old" );
	EXPECT_TRUE( events[1].filename == "other" );
	EXPECT_TRUE( events[2].filename == "new" );
	EXPECT_EQ( 1u, batch.Destinations.count( 7 ) );
}

// A source moved out of the watched directories has no destination: the batch waits for it for
// the time allowed, then ends.
UTEST( InotifyBatch, SourceWithoutDestinationWaitsTheTimeAllowed ) {
	ScriptedSource source;
	source.readAt( 0 );
	source.event( 1, IN_MOVED_FROM, 7, "gone" );
	efsw::InotifyBatch batch;
	ASSERT_TRUE( readBatch( batch, source ) );
	const auto& events = batch.Events;

	ASSERT_EQ( 1u, events.size() );
	EXPECT_EQ( 0u, batch.Destinations.count( 7 ) );
	EXPECT_EQ( efsw::InotifyBatch::MoveDestinationWaitMs, source.Now );
}

// A steady stream of sources without destinations, one read each millisecond: the time allowed
// counts from the first read, so the batch ends on time instead of waiting for the stream to
// stop, and the next batch takes the rest.
UTEST( InotifyBatch, SteadySourcesWithoutDestinationEndTheBatchOnTime ) {
	ScriptedSource source;
	const int reads = 1000;
	for ( int i = 0; i < reads; i++ ) {
		source.readAt( i );
		source.event( 1, IN_MOVED_FROM, 100 + i, "gone" + std::to_string( i ) );
	}
	efsw::InotifyBatch batch;
	ASSERT_TRUE( readBatch( batch, source ) );
	const auto& events = batch.Events;

	EXPECT_LE( source.Now, efsw::InotifyBatch::MoveDestinationWaitMs );
	EXPECT_LE( events.size(),
			   static_cast<size_t>( efsw::InotifyBatch::MoveDestinationWaitMs + 1 ) );
	EXPECT_LT( source.Next, static_cast<size_t>( reads ) );

	const size_t first = events.size();
	ASSERT_TRUE( readBatch( batch, source ) );
	ASSERT_FALSE( events.empty() );
	EXPECT_TRUE( events[0].filename == "gone" + std::to_string( first ) );
}

// A batch without sources reads once, and does not wait.
UTEST( InotifyBatch, BatchWithoutSourcesDoesNotWait ) {
	ScriptedSource source;
	source.readAt( 0 );
	source.event( 1, IN_CREATE, 0, "a" );
	source.event( 2, IN_MOVED_FROM, 7, "old" );
	source.event( 2, IN_MOVED_TO, 7, "new" );
	source.readAt( 0 );
	source.event( 1, IN_CREATE, 0, "b" );
	efsw::InotifyBatch batch;
	ASSERT_TRUE( readBatch( batch, source ) );
	const auto& events = batch.Events;

	EXPECT_EQ( 3u, events.size() );
	EXPECT_EQ( 0, source.Waits );
	EXPECT_EQ( 1u, source.Next );
}

// The watcher shutting down ends a batch without waiting for a destination.
UTEST( InotifyBatch, ShutdownEndsTheWait ) {
	ScriptedSource source;
	source.Running = false;
	source.readAt( 0 );
	source.event( 1, IN_MOVED_FROM, 7, "old" );
	source.readAt( 1 );
	source.event( 2, IN_MOVED_TO, 7, "new" );
	efsw::InotifyBatch batch;
	ASSERT_TRUE( readBatch( batch, source ) );
	const auto& events = batch.Events;

	EXPECT_EQ( 1u, events.size() );
	EXPECT_EQ( 0, source.Waits );
}

// The watcher shutting down while a batch waits: the read it waited for is part of the batch,
// which then ends without waiting again.
UTEST( InotifyBatch, ShutdownDuringTheWaitEndsIt ) {
	ScriptedSource source;
	source.StopAfterWaits = 1;
	source.readAt( 0 );
	source.event( 1, IN_MOVED_FROM, 7, "old" );
	source.readAt( 1 );
	source.event( 1, IN_CREATE, 0, "other" );
	source.readAt( 2 );
	source.event( 2, IN_MOVED_TO, 7, "new" );
	efsw::InotifyBatch batch;
	ASSERT_TRUE( readBatch( batch, source ) );
	const auto& events = batch.Events;

	EXPECT_EQ( 2u, events.size() );
	EXPECT_EQ( 1, source.Waits );
	EXPECT_EQ( 1, source.Now );
}

// A queue overflow, which has no descriptor and no name, is part of the batch like any event.
UTEST( InotifyBatch, OverflowIsAnEvent ) {
	ScriptedSource source;
	source.readAt( 0 );
	source.event( 1, IN_CREATE, 0, "a" );
	source.event( -1, IN_Q_OVERFLOW, 0, "" );
	efsw::InotifyBatch batch;
	ASSERT_TRUE( readBatch( batch, source ) );
	const auto& events = batch.Events;

	ASSERT_EQ( 2u, events.size() );
	EXPECT_EQ( -1, events[1].wd );
	EXPECT_TRUE( ( events[1].mask & IN_Q_OVERFLOW ) != 0 );
	EXPECT_TRUE( events[1].filename.empty() );
}

#endif
