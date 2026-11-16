#ifndef EFSW_TEST_UTIL_HPP
#define EFSW_TEST_UTIL_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <efsw/efsw.hpp>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined( _WIN32 )
#include <process.h>
#else
#include <unistd.h>
#endif

namespace efsw_test {

class TestListener : public efsw::FileWatchListener {
  public:
	std::mutex mtx;
	std::condition_variable cv;
	std::vector<std::tuple<efsw::Action, std::string, std::string>> events;

	void handleFileAction( efsw::WatchID, const std::string& /* dir */, const std::string& filename,
						   efsw::Action action, const std::string& oldFilename = "" ) override {
		std::lock_guard<std::mutex> lock( mtx );
		events.push_back( std::make_tuple( action, filename, oldFilename ) );
		cv.notify_one();
	}

	void clearEvents() {
		std::lock_guard<std::mutex> lock( mtx );
		events.clear();
	}

	bool checkEvent( efsw::Action expectedAction, const std::string& expectedFilename,
					 const std::string& expectedOldFilename = "" ) {
		std::lock_guard<std::mutex> lock( mtx );
		for ( const auto& ev : events ) {
			if ( std::get<0>( ev ) == expectedAction && std::get<1>( ev ) == expectedFilename &&
				 std::get<2>( ev ) == expectedOldFilename ) {
				return true;
			}
		}
		return false;
	}

	/// @return The position of the first matching event, or -1 when absent.
	int indexOf( efsw::Action action, const std::string& filename ) {
		std::lock_guard<std::mutex> lock( mtx );
		for ( size_t i = 0; i < events.size(); ++i ) {
			if ( std::get<0>( events[i] ) == action && std::get<1>( events[i] ) == filename )
				return static_cast<int>( i );
		}
		return -1;
	}

	size_t getEventCount() {
		std::lock_guard<std::mutex> lock( mtx );
		return events.size();
	}

	std::vector<std::tuple<efsw::Action, std::string, std::string>> getEvents() {
		std::lock_guard<std::mutex> lock( mtx );
		return events;
	}

	/// Waits for an event of any of the actions on the file, when backends report a change
	/// differently: one wait, instead of one timeout per action that does not come.
	bool waitForAnyAction( std::initializer_list<efsw::Actions::Action> actions,
						   const std::string& filename, int timeoutMs = 2000 ) {
		std::unique_lock<std::mutex> lock( mtx );
		return cv.wait_for( lock, std::chrono::milliseconds( timeoutMs ), [&] {
			for ( const auto& ev : events )
				for ( efsw::Actions::Action action : actions )
					if ( std::get<0>( ev ) == action && std::get<1>( ev ) == filename )
						return true;
			return false;
		} );
	}

	bool waitForActions( efsw::Actions::Action action, const std::string& filename,
						 int timeoutMs = 2000 ) {
		std::unique_lock<std::mutex> lock( mtx );
		return cv.wait_for( lock, std::chrono::milliseconds( timeoutMs ), [&] {
			for ( const auto& ev : events ) {
				if ( std::get<0>( ev ) == action && std::get<1>( ev ) == filename )
					return true;
			}
			return false;
		} );
	}
};

inline std::string getTemporaryDirectory() {
#if defined( _WIN32 )
	const auto processId = _getpid();
#else
	const auto processId = getpid();
#endif
	return std::filesystem::temp_directory_path().string() + "/efsw_test_" +
		   std::to_string( processId ) + "_" +
		   std::to_string( std::hash<std::thread::id>{}( std::this_thread::get_id() ) );
}

inline bool createDirectory( const std::string& path ) {
	std::error_code ec;
	return std::filesystem::create_directory( path, ec ) || ec.value() == 0;
}

inline bool removeDirectory( const std::string& path ) {
	std::error_code ec;
	std::filesystem::remove_all( path, ec );
	return true;
}

inline bool createFile( const std::string& path, const std::string& content = "" ) {
	std::ofstream f( path );
	if ( !f )
		return false;
	if ( !content.empty() ) {
		f << content;
	}
	f.close();
	return true;
}

inline bool createFileFs( const std::filesystem::path& path, const std::string& content = "" ) {
	std::ofstream f( path );
	if ( !f )
		return false;
	if ( !content.empty() ) {
		f << content;
	}
	f.close();
	return true;
}

inline bool removeFile( const std::string& path ) {
	std::error_code ec;
	std::filesystem::remove( path, ec );
	return !ec;
}

inline bool writeFile( const std::string& path, const std::string& content ) {
	std::ofstream f( path, std::ios::app );
	if ( !f )
		return false;
	f << content;
	f.close();
	return true;
}

inline bool renameFile( const std::string& oldPath, const std::string& newPath ) {
	std::error_code ec;
	std::filesystem::rename( oldPath, newPath, ec );
	return !ec;
}

inline void sleepMs( int ms ) {
	std::this_thread::sleep_for( std::chrono::milliseconds( ms ) );
}

/// Waits until the file watcher has delivered the events of the changes made before the call,
/// instead of sleeping for a while: creates a marker directory in a directory the listener's
/// registration watches, and waits for its Add. A directory, since its creation is a single
/// event. A watch delivers its events in order, so the earlier ones come first, and the
/// registrations of one file watcher share that order, except on Windows, where each directory
/// reports on its own: there a marker in another registration only bounds the wait.
/// @return False when the marker was not reported in time.
inline bool waitForPending( TestListener& listener, const std::string& directory,
							int timeoutMs = 3000 ) {
	static std::atomic<unsigned> markers{ 0 };
	const std::string name = "efsw_marker_" + std::to_string( ++markers );
	return createDirectory( directory + "/" + name ) &&
		   listener.waitForActions( efsw::Actions::Add, name, timeoutMs );
}

/// waitForPending(), for the events of the other registrations of the same file watcher too,
/// such as one that must report nothing more after its removal. Windows reports each directory
/// on its own, and holds Add and Delete events up to 100 ms to pair moves: there that time also
/// passes.
inline bool waitForAllPending( TestListener& listener, const std::string& directory ) {
#if defined( _WIN32 )
	sleepMs( 150 );
#endif
	return waitForPending( listener, directory );
}

/// A registration of its own, for waitForAllPending() in tests whose registrations cannot report
/// a marker: removed ones, or ones whose listener is not a TestListener. Declare it after the
/// file watcher, so that it is removed first.
struct MarkerWatch {
	efsw::FileWatcher& Watcher;
	std::string Directory;
	TestListener Listener;
	efsw::WatchID Id{ 0 };

	MarkerWatch( efsw::FileWatcher& watcher, const std::string& directory ) :
		Watcher( watcher ), Directory( directory ) {
		std::error_code ec;
		std::filesystem::remove_all( Directory, ec );
		std::filesystem::create_directories( Directory, ec );
		Id = Watcher.addWatch( Directory, &Listener, false );
	}

	~MarkerWatch() {
		if ( Id > 0 )
			Watcher.removeWatch( Id );
		std::error_code ec;
		std::filesystem::remove_all( Directory, ec );
	}

	/// @return False when the registration failed, or its marker was not reported in time.
	bool waitForAllPending() {
		return Id > 0 && efsw_test::waitForAllPending( Listener, Directory );
	}
};

extern bool useGeneric;

} // namespace efsw_test

#endif
