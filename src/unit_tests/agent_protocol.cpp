#include "utest.h"
#include <efsw/agent_protocol.hpp>

using namespace efsw::agent;

UTEST( AgentProtocol, FrameRoundTrip ) {
	Writer w;
	w.u8( 7 ).u16( 0xBEEF ).u32( 0xDEADBEEF ).i32( -5 ).str( "path/\xc3\xa1" ).str( "" );
	std::vector<uint8_t> frame = w.frame( MessageWatch );
	ASSERT_EQ( HeaderSize + w.size(), frame.size() );
	// Header is big endian: size, version, type.
	EXPECT_EQ( 0, frame[0] );
	EXPECT_EQ( static_cast<uint8_t>( w.size() ), frame[3] );
	EXPECT_EQ( ProtocolVersion, ( frame[4] << 8 ) | frame[5] );
	EXPECT_EQ( MessageWatch, ( frame[6] << 8 ) | frame[7] );

	FrameParser parser;
	parser.feed( &frame[0], frame.size() );
	Frame out;
	std::string error;
	ASSERT_EQ( 1, parser.next( out, error ) );
	EXPECT_EQ( MessageWatch, out.type );
	Reader r( out.payload );
	EXPECT_EQ( 7, r.u8() );
	EXPECT_EQ( 0xBEEF, r.u16() );
	EXPECT_EQ( 0xDEADBEEFu, r.u32() );
	EXPECT_EQ( -5, r.i32() );
	EXPECT_TRUE( r.str() == "path/\xc3\xa1" );
	EXPECT_TRUE( r.str().empty() );
	EXPECT_TRUE( r.ok() );
	EXPECT_TRUE( r.atEnd() );
	EXPECT_EQ( 0, parser.next( out, error ) );
}

UTEST( AgentProtocol, PartialAndMultipleFrames ) {
	Writer a, b;
	a.u32( 1 );
	b.str( "second" );
	std::vector<uint8_t> stream = a.frame( MessagePing );
	std::vector<uint8_t> second = b.frame( MessageHello );
	stream.insert( stream.end(), second.begin(), second.end() );
	FrameParser parser;
	Frame out;
	std::string error;
	// Byte by byte: frames only appear once complete.
	int frames = 0;
	for ( size_t i = 0; i < stream.size(); ++i ) {
		parser.feed( &stream[i], 1 );
		while ( parser.next( out, error ) == 1 )
			++frames;
	}
	EXPECT_EQ( 2, frames );
	EXPECT_EQ( MessageHello, out.type );
	EXPECT_EQ( 0u, parser.buffered() );
}

UTEST( AgentProtocol, MalformedInput ) {
	// Oversized frame header.
	uint8_t header[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 1, 0, 1 };
	FrameParser parser;
	parser.feed( header, sizeof( header ) );
	Frame out;
	std::string error;
	EXPECT_EQ( -1, parser.next( out, error ) );
	EXPECT_FALSE( error.empty() );

	// A string claiming more bytes than the payload holds.
	Writer w;
	w.u32( 100 );
	Reader r( w.data() );
	std::string s = r.str();
	EXPECT_TRUE( s.empty() );
	EXPECT_FALSE( r.ok() );
	// Further reads stay safe after an overrun.
	EXPECT_EQ( 0u, r.u32() );
	EXPECT_FALSE( r.ok() );
}

UTEST( AgentProtocol, EventPathsAreRelativeToTheWatchedDirectory ) {
	EXPECT_TRUE( EventPath::fromRoot( "/w/root/", "", "/w/root/a/b.txt" ) == "a/b.txt" );
	EXPECT_TRUE( EventPath::fromRoot( "/w/root/", "", "/w/root/dir/" ) == "dir" );
	// Paths reported with the root's symlinks resolved.
	EXPECT_TRUE( EventPath::fromRoot( "/var/root/", "/private/var/root/",
									  "/private/var/root/a.txt" ) == "a.txt" );
	EXPECT_TRUE( EventPath::fromRoot( "/var/root/", "/private/var/root/", "/var/root/a.txt" ) ==
				 "a.txt" );
	EXPECT_FALSE( EventPath::isAbsolute( "a/b.txt" ) );
	EXPECT_FALSE( EventPath::isAbsolute( "" ) );
}

// The source of a move from another watch is outside the watched directory: its old_path stays
// absolute, which clients tell apart from relative paths with isAbsolute().
UTEST( AgentProtocol, MoveFromAnotherWatchKeepsAbsoluteOldPath ) {
	EXPECT_TRUE( EventPath::fromRoot( "/w/destination/", "", "/w/source/a.txt" ) ==
				 "/w/source/a.txt" );
	EXPECT_TRUE( EventPath::isAbsolute( "/w/source/a.txt" ) );
	EXPECT_TRUE( EventPath::fromRoot( "C:/w/destination/", "", "C:/w/source/a.txt" ) ==
				 "C:/w/source/a.txt" );
	EXPECT_TRUE( EventPath::isAbsolute( "C:/w/source/a.txt" ) );
	// A watched directory whose name extends the other one's is not inside it.
	EXPECT_TRUE( EventPath::fromRoot( "/w/dir/", "", "/w/dir2/a.txt" ) == "/w/dir2/a.txt" );
}

UTEST( AgentProtocol, EventBatchRoundTripAndMalformed ) {
	std::vector<WatchEventRecord> records( 2 );
	records[0].action = ActionAdd;
	records[0].path = "a.txt";
	records[1].action = ActionMoved;
	records[1].path = "b.txt";
	records[1].oldPath = "/other/watch/b.txt";
	Writer w;
	EventBatch::write( w, 3, records, 0, records.size() );
	const std::vector<uint8_t> frame = w.frame( MessageEventBatch );
	std::vector<uint8_t> payload( frame.begin() + HeaderSize, frame.end() );

	Reader complete( payload );
	uint32_t watchId = 0;
	std::vector<WatchEventRecord> parsed;
	ASSERT_TRUE( EventBatch::read( complete, watchId, parsed ) );
	EXPECT_EQ( 3u, watchId );
	ASSERT_EQ( 2u, parsed.size() );
	EXPECT_TRUE( parsed[1].oldPath == "/other/watch/b.txt" );

	// A truncated payload, or one with trailing bytes, is malformed.
	std::vector<uint8_t> truncated( payload.begin(), payload.end() - 3 );
	Reader shorter( truncated );
	EXPECT_FALSE( EventBatch::read( shorter, watchId, parsed ) );
	std::vector<uint8_t> longer( payload );
	longer.push_back( 0 );
	Reader trailing( longer );
	EXPECT_FALSE( EventBatch::read( trailing, watchId, parsed ) );
}
