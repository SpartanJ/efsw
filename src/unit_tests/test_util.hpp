#ifndef EFSW_TEST_UTIL_HPP
#define EFSW_TEST_UTIL_HPP

#include <efsw/efsw.hpp>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>

namespace efsw_test {

class TestListener : public efsw::FileWatchListener {
  public:
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::tuple<efsw::Action, std::string, std::string>> events;

    void handleFileAction( efsw::WatchID watchid, const std::string& dir,
                           const std::string& filename, efsw::Action action,
                           std::string oldFilename = "" ) override {
        std::lock_guard<std::mutex> lock( mtx );
        events.push_back( std::make_tuple( action, filename, oldFilename ) );
        cv.notify_one();
    }

    bool waitForEvents( size_t count, int timeoutMs = 500 ) {
        std::unique_lock<std::mutex> lock( mtx );
        return cv.wait_for( lock, std::chrono::milliseconds( timeoutMs ),
                            [ this, count ]() { return events.size() >= count; } );
    }

    void clearEvents() {
        std::lock_guard<std::mutex> lock( mtx );
        events.clear();
    }

    bool checkEvent( efsw::Action expectedAction, const std::string& expectedFilename, const std::string& expectedOldFilename = "" ) {
        std::lock_guard<std::mutex> lock( mtx );
        for ( const auto& ev : events ) {
            if ( std::get<0>( ev ) == expectedAction &&
                 std::get<1>( ev ) == expectedFilename &&
                 std::get<2>( ev ) == expectedOldFilename ) {
                return true;
            }
        }
        return false;
    }

    size_t getEventCount() {
        std::lock_guard<std::mutex> lock( mtx );
        return events.size();
    }

    std::vector<std::tuple<efsw::Action, std::string, std::string>> getEvents() {
        std::lock_guard<std::mutex> lock( mtx );
        return events;
    }

    bool waitForActions( efsw::Actions::Action action, const std::string& filename, int timeoutMs = 2000 ) {
        auto start = std::chrono::steady_clock::now();
        while ( true ) {
            {
                std::lock_guard<std::mutex> lock( mtx );
                for ( const auto& ev : events ) {
                    if ( std::get<0>( ev ) == action &&
                         std::get<1>( ev ) == filename ) {
                        return true;
                    }
                }
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start ).count();
            if ( elapsed >= timeoutMs ) {
                return false;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }
        return false;
    }
};

inline std::string getTemporaryDirectory() {
    return std::filesystem::temp_directory_path().string() + "/efsw_test_" + std::to_string( std::hash<std::thread::id>{}( std::this_thread::get_id() ) );
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
    if ( !f ) return false;
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
    if ( !f ) return false;
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

inline void cleanupWatcher( efsw::FileWatcher& fileWatcher ) {
    sleepMs( 200 );
}

}

#endif
