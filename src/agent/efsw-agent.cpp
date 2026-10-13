/**
	efsw-agent: exposes efsw directory watching over stdin/stdout.

	Usage: efsw-agent --stdio [--polling-ms N]
	       efsw-agent --version

	See include/efsw/agent_protocol.hpp for the wire protocol. The agent is meant to be started on
	a remote machine through an SSH exec channel. It never reads or writes file contents; it only
	reports changes, and it exits when its input stream is closed.
*/
#include <efsw/agent_protocol.hpp>
#include <efsw/efsw.hpp>

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined( _WIN32 )
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace efsw::agent;

namespace {

const char* AgentVersion = "1.0.0";

const char* platformName() {
#if defined( _WIN32 )
	return "windows";
#elif defined( __APPLE__ )
	return "macos";
#elif defined( __linux__ )
	return "linux";
#elif defined( __FreeBSD__ )
	return "freebsd";
#elif defined( __HAIKU__ )
	return "haiku";
#else
	return "unknown";
#endif
}

bool readInput( void* buffer, size_t size, size_t& got ) {
#if defined( _WIN32 )
	int n = _read( 0, buffer, static_cast<unsigned>( size ) );
#else
	ssize_t n = ::read( 0, buffer, size );
	while ( n < 0 && errno == EINTR )
		n = ::read( 0, buffer, size );
#endif
	if ( n <= 0 )
		return false;
	got = static_cast<size_t>( n );
	return true;
}

bool writeOutput( const std::vector<uint8_t>& data ) {
	size_t done = 0;
	while ( done < data.size() ) {
#if defined( _WIN32 )
		int n = _write( 1, &data[done], static_cast<unsigned>( data.size() - done ) );
#else
		ssize_t n = ::write( 1, &data[done], data.size() - done );
		if ( n < 0 && errno == EINTR )
			continue;
#endif
		if ( n <= 0 )
			return false;
		done += static_cast<size_t>( n );
	}
	return true;
}

std::string normalizeSlashes( std::string path ) {
#if defined( _WIN32 )
	for ( size_t i = 0; i < path.size(); ++i )
		if ( path[i] == '\\' )
			path[i] = '/';
#endif
	return path;
}

class Agent;

/** One listener per efsw::FileWatcher: watch ids are only unique within a watcher. */
class Listener : public efsw::FileWatchListener {
  public:
	Listener( Agent& agent, int key ) : mAgent( agent ), mKey( key ) {}

	void handleFileAction( efsw::WatchID watchid, const std::string& dir,
						   const std::string& filename, efsw::Action action,
						   const std::string& oldFilename ) override;

	void handleMissedFileActions( efsw::WatchID watchid, const std::string& ) override;

  private:
	Agent& mAgent;
	int mKey;
};

class Agent {
  public:
	explicit Agent( unsigned pollingMs ) : mPollingMs( pollingMs ), mStop( false ) {
		mWriter = std::thread( &Agent::writerLoop, this );
	}

	~Agent() {
		// Watchers must stop (joining their threads) before listeners and state go away.
		{
			std::lock_guard<std::mutex> lock( mWatchersMutex );
			for ( std::map<int, Slot>::iterator it = mWatchers.begin(); it != mWatchers.end();
				  ++it )
				it->second.watcher.reset();
		}
		{
			std::lock_guard<std::mutex> lock( mOutMutex );
			mStop = true;
		}
		mOutReady.notify_all();
		if ( mWriter.joinable() )
			mWriter.join();
	}

	/** Runs until stdin closes or the protocol is violated. @return Process exit code. */
	int run() {
		FrameParser parser;
		std::vector<uint8_t> buffer( 64 * 1024 );
		Frame frame;
		std::string error;
		while ( true ) {
			size_t got = 0;
			if ( !readInput( &buffer[0], buffer.size(), got ) )
				return 0;
			parser.feed( &buffer[0], got );
			int result;
			while ( ( result = parser.next( frame, error ) ) == 1 ) {
				if ( !handle( frame, error ) ) {
					sendError( error );
					return 2;
				}
			}
			if ( result < 0 ) {
				sendError( error );
				return 2;
			}
		}
	}

	void onEvent( int key, efsw::WatchID efswId, const std::string& dir,
				  const std::string& filename, efsw::Action action,
				  const std::string& oldFilename ) {
		uint32_t watchId;
		std::string root;
		if ( !lookup( key, efswId, watchId, root ) )
			return;
		WatchEventRecord record;
		record.action = static_cast<uint8_t>( action );
		record.path = relative( root, dir + filename );
		if ( action == efsw::Actions::Moved ) {
			bool absolute = !oldFilename.empty() &&
							( oldFilename[0] == '/' ||
							  ( oldFilename.size() > 1 && oldFilename[1] == ':' ) );
			record.oldPath = relative( root, absolute ? oldFilename : dir + oldFilename );
		}
		{
			std::lock_guard<std::mutex> lock( mOutMutex );
			mPendingEvents[watchId].push_back( record );
		}
		mOutReady.notify_one();
	}

	void onMissed( int key, efsw::WatchID efswId ) {
		uint32_t watchId;
		std::string root;
		if ( !lookup( key, efswId, watchId, root ) )
			return;
		{
			std::lock_guard<std::mutex> lock( mOutMutex );
			// A rescan supersedes every pending incremental event of this watch.
			mPendingEvents.erase( watchId );
			mPendingMissed.push_back( watchId );
		}
		mOutReady.notify_one();
	}

  private:
	struct Slot {
		std::unique_ptr<Listener> listener;
		std::unique_ptr<efsw::FileWatcher> watcher;
	};

	struct Watch {
		int key;
		efsw::WatchID efswId;
		std::string root;
	};

	static std::string relative( const std::string& root, const std::string& path ) {
		std::string normalized = normalizeSlashes( path );
		if ( normalized.compare( 0, root.size(), root ) == 0 )
			normalized.erase( 0, root.size() );
		while ( !normalized.empty() && normalized[normalized.size() - 1] == '/' )
			normalized.erase( normalized.size() - 1 );
		return normalized;
	}

	/** Events of an efsw watch that is not registered yet are dropped: they happened before
	 * WATCH_OK was sent, and clients list the directory only after WATCH_OK. */
	bool lookup( int key, efsw::WatchID efswId, uint32_t& watchId, std::string& root ) {
		std::lock_guard<std::mutex> lock( mWatchesMutex );
		std::map<std::pair<int, efsw::WatchID>, uint32_t>::iterator it =
			mByEfsw.find( std::make_pair( key, efswId ) );
		if ( it == mByEfsw.end() )
			return false;
		watchId = it->second;
		root = mWatches[watchId].root;
		return true;
	}

	void send( uint16_t type, const Writer& payload ) {
		{
			std::lock_guard<std::mutex> lock( mOutMutex );
			mOutgoing.push_back( payload.frame( type ) );
		}
		mOutReady.notify_one();
	}

	void sendError( const std::string& message ) {
		Writer w;
		w.str( message );
		send( MessageError, w );
	}

	efsw::WatchID addWatch( int key, const std::string& path, uint8_t flags ) {
		std::lock_guard<std::mutex> lock( mWatchersMutex );
		Slot& slot = mWatchers[key];
		if ( !slot.watcher ) {
			slot.listener.reset( new Listener( *this, key ) );
			slot.watcher.reset( ( key & 1 ) ? new efsw::FileWatcher( true, mPollingMs )
											: new efsw::FileWatcher() );
			slot.watcher->followSymlinks( ( flags & WatchFollowSymlinks ) != 0 );
			slot.watcher->allowOutOfScopeLinks( ( flags & WatchAllowOutOfScopeLinks ) != 0 );
			slot.watcher->watch();
		}
		std::vector<efsw::WatcherOption> options;
		if ( flags & WatchReportCrossDirectoryMoves )
			options.push_back( efsw::WatcherOption( efsw::Options::ReportCrossDirectoryMoves, 1 ) );
		return slot.watcher->addWatch( path, slot.listener.get(), ( flags & WatchRecursive ) != 0,
									   options );
	}

	bool handle( const Frame& frame, std::string& error ) {
		if ( frame.version != ProtocolVersion ) {
			error = "unsupported protocol version";
			return false;
		}
		Reader r( frame.payload );
		switch ( frame.type ) {
			case MessageHello: {
				r.str();
				Writer w;
				w.u16( ProtocolVersion ).str( AgentVersion ).str( platformName() );
				w.u32( CapabilityNative | CapabilityGeneric );
				send( MessageHelloReply, w );
				break;
			}
			case MessageWatch: {
				uint32_t id = r.u32();
				std::string path = r.str();
				uint8_t flags = r.u8();
				uint32_t polling = r.u32();
				if ( !r.ok() ) {
					error = "malformed WATCH";
					return false;
				}
				if ( polling > 0 )
					mPollingMs = polling;
				std::string root = normalizeSlashes( path );
				if ( root.empty() || root[root.size() - 1] != '/' )
					root += '/';
				int optionBits = ( ( flags & WatchFollowSymlinks ) ? 2 : 0 ) |
								 ( ( flags & WatchAllowOutOfScopeLinks ) ? 4 : 0 );
				int key = optionBits;
				efsw::WatchID efswId = efsw::Errors::FileRemote;
				if ( !( flags & WatchForceGeneric ) )
					efswId = addWatch( key, path, flags );
				if ( efswId == efsw::Errors::FileRemote ||
					 efswId == efsw::Errors::FileOverlapping ||
					 efswId == efsw::Errors::FileRepeated ||
					 efswId == efsw::Errors::WatcherFailed ) {
					// Network mounts on the remote host are polled locally there, not over SSH.
					key = optionBits | 1;
					efswId = addWatch( key, path, flags );
				}
				if ( efswId < 0 ) {
					Writer w;
					w.u32( id ).i32( static_cast<int32_t>( efswId ) );
					w.str( efsw::Errors::Log::getLastErrorLog() );
					send( MessageWatchError, w );
					break;
				}
				{
					std::lock_guard<std::mutex> lock( mWatchesMutex );
					Watch watch = { key, efswId, root };
					mWatches[id] = watch;
					mByEfsw[std::make_pair( key, efswId )] = id;
				}
				Writer w;
				w.u32( id ).u8( ( key & 1 ) ? BackendGeneric : BackendNative );
				send( MessageWatchOk, w );
				break;
			}
			case MessageUnwatch: {
				uint32_t id = r.u32();
				if ( !r.ok() ) {
					error = "malformed UNWATCH";
					return false;
				}
				Watch watch;
				bool found = false;
				{
					std::lock_guard<std::mutex> lock( mWatchesMutex );
					std::map<uint32_t, Watch>::iterator it = mWatches.find( id );
					if ( it != mWatches.end() ) {
						watch = it->second;
						mByEfsw.erase( std::make_pair( watch.key, watch.efswId ) );
						mWatches.erase( it );
						found = true;
					}
				}
				if ( found ) {
					std::lock_guard<std::mutex> lock( mWatchersMutex );
					std::map<int, Slot>::iterator slot = mWatchers.find( watch.key );
					if ( slot != mWatchers.end() && slot->second.watcher )
						slot->second.watcher->removeWatch( watch.efswId );
				}
				{
					std::lock_guard<std::mutex> lock( mOutMutex );
					mPendingEvents.erase( id );
				}
				Writer w;
				w.u32( id );
				send( MessageUnwatchOk, w );
				break;
			}
			case MessagePing: {
				uint32_t nonce = r.u32();
				Writer w;
				w.u32( nonce );
				send( MessagePong, w );
				break;
			}
			default:
				error = "unknown message type";
				return false;
		}
		return true;
	}

	/** Single owner of stdout. Each wake-up flushes control replies and coalesces every pending
	 * event of a watch into one EVENT_BATCH frame. */
	void writerLoop() {
		while ( true ) {
			std::deque<std::vector<uint8_t> > outgoing;
			std::map<uint32_t, std::vector<WatchEventRecord> > events;
			std::vector<uint32_t> missed;
			{
				std::unique_lock<std::mutex> lock( mOutMutex );
				while ( !mStop && mOutgoing.empty() && mPendingEvents.empty() &&
						mPendingMissed.empty() )
					mOutReady.wait( lock );
				if ( mStop && mOutgoing.empty() && mPendingEvents.empty() &&
					 mPendingMissed.empty() )
					return;
				outgoing.swap( mOutgoing );
				events.swap( mPendingEvents );
				missed.swap( mPendingMissed );
			}
			for ( size_t i = 0; i < outgoing.size(); ++i )
				if ( !writeOutput( outgoing[i] ) )
					std::exit( 3 );
			for ( size_t i = 0; i < missed.size(); ++i ) {
				Writer w;
				w.u32( missed[i] );
				if ( !writeOutput( w.frame( MessageMissedEvents ) ) )
					std::exit( 3 );
			}
			for ( std::map<uint32_t, std::vector<WatchEventRecord> >::iterator it = events.begin();
				  it != events.end(); ++it ) {
				// Split very large bursts to stay far below the frame limit.
				const std::vector<WatchEventRecord>& list = it->second;
				for ( size_t start = 0; start < list.size(); start += 4096 ) {
					size_t end = start + 4096 < list.size() ? start + 4096 : list.size();
					Writer w;
					w.u32( it->first ).u32( static_cast<uint32_t>( end - start ) );
					for ( size_t i = start; i < end; ++i )
						w.u8( list[i].action ).str( list[i].path ).str( list[i].oldPath );
					if ( !writeOutput( w.frame( MessageEventBatch ) ) )
						std::exit( 3 );
				}
			}
		}
	}

	unsigned mPollingMs;
	std::mutex mWatchersMutex;
	std::map<int, Slot> mWatchers;
	std::mutex mWatchesMutex;
	std::map<uint32_t, Watch> mWatches;
	std::map<std::pair<int, efsw::WatchID>, uint32_t> mByEfsw;
	std::mutex mOutMutex;
	std::condition_variable mOutReady;
	std::deque<std::vector<uint8_t> > mOutgoing;
	std::map<uint32_t, std::vector<WatchEventRecord> > mPendingEvents;
	std::vector<uint32_t> mPendingMissed;
	bool mStop;
	std::thread mWriter;
};

void Listener::handleFileAction( efsw::WatchID watchid, const std::string& dir,
								 const std::string& filename, efsw::Action action,
								 const std::string& oldFilename ) {
	mAgent.onEvent( mKey, watchid, dir, filename, action, oldFilename );
}

void Listener::handleMissedFileActions( efsw::WatchID watchid, const std::string& ) {
	mAgent.onMissed( mKey, watchid );
}

} // namespace

int main( int argc, char** argv ) {
	bool stdio = false;
	unsigned pollingMs = 1000;
	for ( int i = 1; i < argc; ++i ) {
		if ( std::strcmp( argv[i], "--stdio" ) == 0 ) {
			stdio = true;
		} else if ( std::strcmp( argv[i], "--version" ) == 0 ) {
			std::printf( "efsw-agent %s protocol %u %s\n", AgentVersion,
						 static_cast<unsigned>( ProtocolVersion ), platformName() );
			return 0;
		} else if ( std::strcmp( argv[i], "--polling-ms" ) == 0 && i + 1 < argc ) {
			pollingMs = static_cast<unsigned>( std::atoi( argv[++i] ) );
		}
	}
	if ( !stdio ) {
		std::fprintf( stderr, "usage: efsw-agent --stdio [--polling-ms N] | --version\n" );
		return 1;
	}
#if defined( _WIN32 )
	_setmode( 0, _O_BINARY );
	_setmode( 1, _O_BINARY );
#endif
	Agent agent( pollingMs );
	return agent.run();
}
