cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

project(efsw)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 11)

set(ESFW_MAIN_PROJECT OFF)

if((CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR) AND(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME))
	set(ESFW_MAIN_PROJECT ON)
endif()

include(GNUInstallDirs)

find_package(Threads REQUIRED)

option(VERBOSE "Build efsw with verbose mode.")
option(LEGACY "Build efsw with cpp legacy support (before C++11).")
option(BUILD_SHARED_LIBS "Build efsw as a shared library" ON)
option(BUILD_STATIC_LIBS "Build efsw as a static library" ON)
option(BUILD_TEST_APP "Build the test app" ${ESFW_MAIN_PROJECT})
option(EFSW_INSTALL "Add efsw install targets" ${ESFW_MAIN_PROJECT})

add_library(efsw)

if(BUILD_STATIC_LIBS)
	add_library(efsw-static STATIC)

	target_include_directories(efsw-static
		PRIVATE src/
		PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
	)
endif()

set(EFSW_CPP_SOURCE
	src/efsw/Debug.cpp
	src/efsw/DirectorySnapshot.cpp
	src/efsw/DirectorySnapshotDiff.cpp
	src/efsw/DirWatcherGeneric.cpp
	src/efsw/FileInfo.cpp
	src/efsw/FileSystem.cpp
	src/efsw/FileWatcher.cpp
	src/efsw/FileWatcherCWrapper.cpp
	src/efsw/FileWatcherGeneric.cpp
	src/efsw/FileWatcherImpl.cpp
	src/efsw/Log.cpp
	src/efsw/Mutex.cpp
	src/efsw/String.cpp
	src/efsw/System.cpp
	src/efsw/Thread.cpp
	src/efsw/Watcher.cpp
	src/efsw/WatcherGeneric.cpp
)

target_include_directories(efsw
	PRIVATE src/
	PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(VERBOSE)
	target_compile_definitions(efsw PRIVATE EFSW_VERBOSE)
endif()

if(NOT LEGACY)
	target_compile_features(efsw PRIVATE cxx_std_11)
else()
	target_compile_definitions(efsw PRIVATE EFSW_LEGACY_CPP)
endif()

if(BUILD_SHARED_LIBS)
	target_compile_definitions(efsw PRIVATE EFSW_DYNAMIC EFSW_EXPORTS)
endif()

# platforms
if(WIN32)
	list(APPEND EFSW_CPP_SOURCE
		src/efsw/platform/win/FileSystemImpl.cpp
		src/efsw/platform/win/MutexImpl.cpp
		src/efsw/platform/win/SystemImpl.cpp
		src/efsw/platform/win/ThreadImpl.cpp
	)
else()
	list(APPEND EFSW_CPP_SOURCE
		src/efsw/platform/posix/FileSystemImpl.cpp
		src/efsw/platform/posix/MutexImpl.cpp
		src/efsw/platform/posix/SystemImpl.cpp
		src/efsw/platform/posix/ThreadImpl.cpp
	)
endif()

# watcher implementations
if(APPLE)
	list(APPEND EFSW_CPP_SOURCE
		src/efsw/FileWatcherFSEvents.cpp
		src/efsw/FileWatcherKqueue.cpp
		src/efsw/WatcherFSEvents.cpp
		src/efsw/WatcherKqueue.cpp
	)

	if(NOT CMAKE_SYSTEM_VERSION GREATER 9)
		target_compile_definitions(efsw PRIVATE EFSW_FSEVENTS_NOT_SUPPORTED)
	endif()
elseif(WIN32)
	list(APPEND EFSW_CPP_SOURCE
		src/efsw/FileWatcherWin32.cpp
		src/efsw/WatcherWin32.cpp
	)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
	list(APPEND EFSW_CPP_SOURCE
		src/efsw/FileWatcherInotify.cpp
		src/efsw/WatcherInotify.cpp
	)

	if(NOT EXISTS "/usr/include/sys/inotify.h" AND NOT EXISTS "/usr/local/include/sys/inotify.h")
		target_compile_definitions(efsw PRIVATE EFSW_INOTIFY_NOSYS)
	endif()
elseif(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
	list(APPEND EFSW_CPP_SOURCE
		src/efsw/FileWatcherKqueue.cpp
		src/efsw/WatcherKqueue.cpp
	)
endif()

if(MSVC)
	target_compile_definitions(efsw PRIVATE _SCL_SECURE_NO_WARNINGS)
else()
	target_compile_options(efsw PRIVATE -Wall -Wno-long-long -fPIC)
endif()

target_compile_definitions(efsw PRIVATE $<IF:$<CONFIG:Debug>,DEBUG,NDEBUG>)

if(APPLE)
	set(MAC_LIBS "-framework CoreFoundation" "-framework CoreServices")
	target_link_libraries(efsw PRIVATE ${MAC_LIBS})
elseif(NOT(${CMAKE_SYSTEM_NAME} MATCHES "Haiku") AND NOT WIN32)
	target_link_libraries(efsw PRIVATE Threads::Threads)
endif()

target_sources(efsw PRIVATE ${EFSW_CPP_SOURCE})

if(BUILD_STATIC_LIBS)
	target_sources(efsw-static PRIVATE ${EFSW_CPP_SOURCE})
endif()

include(CMakePackageConfigHelpers)

set(packageDestDir "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}")

configure_package_config_file(
	${CMAKE_CURRENT_SOURCE_DIR}/efswConfig.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/cmake/efswConfig.cmake
	INSTALL_DESTINATION "${packageDestDir}"
	NO_SET_AND_CHECK_MACRO
	NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

export(TARGETS efsw NAMESPACE efsw:: FILE ${CMAKE_CURRENT_BINARY_DIR}/cmake/${PROJECT_NAME}Targets.cmake)
export(TARGETS efsw-static NAMESPACE efsw:: FILE ${CMAKE_CURRENT_BINARY_DIR}/cmake/${PROJECT_NAME}-staticTargets.cmake)

if(EFSW_INSTALL)
	install(TARGETS efsw EXPORT efswExport
		LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
		ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	)

	install(
		FILES
		include/efsw/efsw.h include/efsw/efsw.hpp
		DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/efsw
	)

	install(EXPORT efswExport NAMESPACE efsw:: DESTINATION "${packageDestDir}" FILE ${PROJECT_NAME}Targets.cmake)
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cmake/efswConfig.cmake DESTINATION "${packageDestDir}")

	if(BUILD_SHARED_LIBS)
		install(TARGETS efsw-static EXPORT efswExport
			LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
			ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
			RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
		)

		install(
			FILES
			include/efsw/efsw.h include/efsw/efsw.hpp
			DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/efsw
		)

		install(EXPORT efswExport NAMESPACE efsw:: DESTINATION "${packageDestDir}" FILE ${PROJECT_NAME}-staticTargets.cmake)
		install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cmake/efswConfig.cmake DESTINATION "${packageDestDir}")
	endif()
endif()

if(BUILD_TEST_APP)
	# C++ test application
	add_executable(efsw-test src/test/efsw-test.cpp)
	target_link_libraries(efsw-test efsw-static)

	# C test application
	add_executable(efsw-test-stdc src/test/efsw-test.c)
	target_link_libraries(efsw-test-stdc efsw-static)
endif()
