/**
	efsw-agent wire protocol.

	The agent ("efsw-agent --stdio") exposes efsw file watching over stdin/stdout so a client can
	watch directories on another machine through any byte stream, typically an SSH exec channel.
	It is not an I/O proxy: it only reports filesystem changes.

	Framing (all integers in network byte order):

		uint32 payload_size   bytes following this 8-byte header
		uint16 protocol_version
		uint16 message_type
		byte[payload_size]

	Strings are encoded as uint32 byte_length followed by that many UTF-8 bytes (no terminator).
	Paths always use '/' as separator; event paths are relative to the watched directory.

	Messages (client -> agent: C, agent -> client: A):

		HELLO          C  string client_name
		HELLO_REPLY    A  uint16 protocol_version, string agent_version, string platform,
		                  uint32 capabilities (CapabilityNative | CapabilityGeneric)
		WATCH          C  uint32 watch_id, string path, uint8 flags (WatchFlag*),
		                  uint32 generic_polling_interval_ms
		WATCH_OK       A  uint32 watch_id, uint8 backend (BackendNative | BackendGeneric)
		WATCH_ERROR    A  uint32 watch_id, int32 efsw_error, string message
		UNWATCH        C  uint32 watch_id
		UNWATCH_OK     A  uint32 watch_id
		EVENT_BATCH    A  uint32 watch_id, uint32 count,
		                  count x { uint8 action, string path, string old_path }
		MISSED_EVENTS  A  uint32 watch_id
		PING           C  uint32 nonce
		PONG           A  uint32 nonce
		ERROR          A  string message (fatal protocol error; the agent exits)

	Platform specific efsw option integers are never sent: options are expressed through the
	portable WatchFlag bits. The agent exits when stdin reaches EOF.

	This header is self-contained (C++11, no efsw dependency) so clients can include it alone.
*/
#ifndef EFSW_AGENT_PROTOCOL_HPP
#define EFSW_AGENT_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace efsw { namespace agent {

static const uint16_t ProtocolVersion = 1;

/** Frames larger than this are rejected as malformed. */
static const uint32_t MaxPayloadSize = 16u * 1024u * 1024u;

static const size_t HeaderSize = 8;

enum MessageType {
	MessageHello = 1,
	MessageHelloReply = 2,
	MessageWatch = 3,
	MessageWatchOk = 4,
	MessageWatchError = 5,
	MessageUnwatch = 6,
	MessageUnwatchOk = 7,
	MessageEventBatch = 8,
	MessageMissedEvents = 9,
	MessagePing = 10,
	MessagePong = 11,
	MessageError = 12
};

enum EventAction { ActionAdd = 1, ActionDelete = 2, ActionModified = 3, ActionMoved = 4 };

enum WatchBackend { BackendNative = 0, BackendGeneric = 1 };

enum Capability { CapabilityNative = 1u << 0, CapabilityGeneric = 1u << 1 };

enum WatchFlag {
	WatchRecursive = 1u << 0,
	WatchFollowSymlinks = 1u << 1,
	WatchAllowOutOfScopeLinks = 1u << 2,
	WatchReportCrossDirectoryMoves = 1u << 3,
	/** Skip the native watcher and poll with the generic watcher directly. */
	WatchForceGeneric = 1u << 4
};

struct Frame {
	uint16_t version;
	uint16_t type;
	std::vector<uint8_t> payload;

	Frame() : version( 0 ), type( 0 ) {}
};

/** Serializes one payload; frame() prepends the header. */
class Writer {
  public:
	Writer& u8( uint8_t value ) {
		mData.push_back( value );
		return *this;
	}

	Writer& u16( uint16_t value ) {
		mData.push_back( static_cast<uint8_t>( value >> 8 ) );
		mData.push_back( static_cast<uint8_t>( value ) );
		return *this;
	}

	Writer& u32( uint32_t value ) {
		mData.push_back( static_cast<uint8_t>( value >> 24 ) );
		mData.push_back( static_cast<uint8_t>( value >> 16 ) );
		mData.push_back( static_cast<uint8_t>( value >> 8 ) );
		mData.push_back( static_cast<uint8_t>( value ) );
		return *this;
	}

	Writer& i32( int32_t value ) { return u32( static_cast<uint32_t>( value ) ); }

	Writer& str( const std::string& value ) {
		u32( static_cast<uint32_t>( value.size() ) );
		mData.insert( mData.end(), value.begin(), value.end() );
		return *this;
	}

	const std::vector<uint8_t>& data() const { return mData; }

	size_t size() const { return mData.size(); }

	/** @return The complete frame (header + payload). */
	std::vector<uint8_t> frame( uint16_t type ) const {
		std::vector<uint8_t> out;
		out.reserve( HeaderSize + mData.size() );
		uint32_t size = static_cast<uint32_t>( mData.size() );
		out.push_back( static_cast<uint8_t>( size >> 24 ) );
		out.push_back( static_cast<uint8_t>( size >> 16 ) );
		out.push_back( static_cast<uint8_t>( size >> 8 ) );
		out.push_back( static_cast<uint8_t>( size ) );
		out.push_back( static_cast<uint8_t>( ProtocolVersion >> 8 ) );
		out.push_back( static_cast<uint8_t>( ProtocolVersion ) );
		out.push_back( static_cast<uint8_t>( type >> 8 ) );
		out.push_back( static_cast<uint8_t>( type ) );
		out.insert( out.end(), mData.begin(), mData.end() );
		return out;
	}

  private:
	std::vector<uint8_t> mData;
};

/** Bounds-checked payload reader. Any overrun sets ok() to false and yields zero values. */
class Reader {
  public:
	Reader( const uint8_t* data, size_t size ) : mData( data ), mSize( size ), mPos( 0 ), mOk( true ) {}

	explicit Reader( const std::vector<uint8_t>& data ) :
		mData( data.empty() ? NULL : &data[0] ), mSize( data.size() ), mPos( 0 ), mOk( true ) {}

	bool ok() const { return mOk; }

	bool atEnd() const { return mPos == mSize; }

	uint8_t u8() {
		if ( !need( 1 ) )
			return 0;
		return mData[mPos++];
	}

	uint16_t u16() {
		if ( !need( 2 ) )
			return 0;
		uint16_t value = static_cast<uint16_t>( ( mData[mPos] << 8 ) | mData[mPos + 1] );
		mPos += 2;
		return value;
	}

	uint32_t u32() {
		if ( !need( 4 ) )
			return 0;
		uint32_t value = ( static_cast<uint32_t>( mData[mPos] ) << 24 ) |
						 ( static_cast<uint32_t>( mData[mPos + 1] ) << 16 ) |
						 ( static_cast<uint32_t>( mData[mPos + 2] ) << 8 ) |
						 static_cast<uint32_t>( mData[mPos + 3] );
		mPos += 4;
		return value;
	}

	int32_t i32() { return static_cast<int32_t>( u32() ); }

	std::string str() {
		uint32_t length = u32();
		if ( !mOk || !need( length ) )
			return std::string();
		std::string value( reinterpret_cast<const char*>( mData + mPos ), length );
		mPos += length;
		return value;
	}

  private:
	bool need( size_t count ) {
		if ( !mOk || mSize - mPos < count ) {
			mOk = false;
			return false;
		}
		return true;
	}

	const uint8_t* mData;
	size_t mSize;
	size_t mPos;
	bool mOk;
};

/** Incremental frame decoder for a byte stream. */
class FrameParser {
  public:
	FrameParser() : mOffset( 0 ) {}

	void feed( const void* data, size_t size ) {
		const uint8_t* bytes = static_cast<const uint8_t*>( data );
		mBuffer.insert( mBuffer.end(), bytes, bytes + size );
	}

	/** Extracts the next complete frame.
	 * @return 1 when a frame was produced, 0 when more data is needed, -1 on a malformed stream
	 * (error describes it; the stream must be dropped). */
	int next( Frame& frame, std::string& error ) {
		size_t available = mBuffer.size() - mOffset;
		if ( available < HeaderSize ) {
			compact();
			return 0;
		}
		const uint8_t* p = &mBuffer[mOffset];
		uint32_t size = ( static_cast<uint32_t>( p[0] ) << 24 ) |
						( static_cast<uint32_t>( p[1] ) << 16 ) |
						( static_cast<uint32_t>( p[2] ) << 8 ) | static_cast<uint32_t>( p[3] );
		if ( size > MaxPayloadSize ) {
			error = "frame too large";
			return -1;
		}
		if ( available < HeaderSize + size ) {
			compact();
			return 0;
		}
		frame.version = static_cast<uint16_t>( ( p[4] << 8 ) | p[5] );
		frame.type = static_cast<uint16_t>( ( p[6] << 8 ) | p[7] );
		frame.payload.assign( p + HeaderSize, p + HeaderSize + size );
		mOffset += HeaderSize + size;
		return 1;
	}

	size_t buffered() const { return mBuffer.size() - mOffset; }

  private:
	void compact() {
		if ( mOffset > 0 ) {
			mBuffer.erase( mBuffer.begin(), mBuffer.begin() + mOffset );
			mOffset = 0;
		}
	}

	std::vector<uint8_t> mBuffer;
	size_t mOffset;
};

struct WatchEventRecord {
	uint8_t action;
	std::string path;
	std::string oldPath;
};

}} // namespace efsw::agent

#endif
