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
