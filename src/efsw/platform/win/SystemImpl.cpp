#include <efsw/platform/win/SystemImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace efsw { namespace Platform {

void System::sleep( const unsigned long& ms ) {
	::Sleep( ms );
}

}}

#endif
