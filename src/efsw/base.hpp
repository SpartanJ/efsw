#ifndef EFSW_BASE
#define EFSW_BASE

#include <efsw/sophist.h>
#include <efsw/efsw.hpp>

namespace efsw {
typedef SOPHIST_int8		Int8;
typedef SOPHIST_uint8		Uint8;
typedef SOPHIST_int16		Int16;
typedef SOPHIST_uint16		Uint16;
typedef SOPHIST_int32		Int32;
typedef SOPHIST_uint32		Uint32;
typedef SOPHIST_uint64		Uint64;
}

#define EFSW_PLATFORM_WIN32		1
#define EFSW_PLATFORM_INOTIFY	2
#define EFSW_PLATFORM_KQUEUE	3
#define EFSW_PLATFORM_GENERIC	4

#if defined(_WIN32)
///	Any Windows platform
#	define EFSW_PLATFORM EFSW_PLATFORM_WIN32

	#if ( defined( _MSCVER ) || defined( _MSC_VER ) )
		#define EFSW_COMPILER_MSVC
	#endif
#elif defined(__APPLE_CC__) || defined( __FreeBSD__ ) || defined(__OpenBSD__) || defined( __NetBSD__ ) || defined( __DragonFly__ )
///	This includes OS X, iOS and BSD
#	define EFSW_PLATFORM EFSW_PLATFORM_KQUEUE
#elif defined(__linux__)
///	This includes Linux and Android
#	define EFSW_PLATFORM EFSW_PLATFORM_INOTIFY
#else
///	Everything else
#	define EFSW_PLATFORM EFSW_PLATFORM_GENERIC
#endif

#if EFSW_PLATFORM != EFSW_PLATFORM_WIN32
	#define EFSW_PLATFORM_POSIX
#endif

#define efCOMMA ,

#define efSAFE_DELETE(p)		{ if(p) { delete (p);			(p)=NULL; } }

#endif
