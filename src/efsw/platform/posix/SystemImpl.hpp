#ifndef EFSW_SYSTEMIMPLPOSIX_HPP
#define EFSW_SYSTEMIMPLPOSIX_HPP

#include <efsw/base.hpp>

#if defined( EFSW_PLATFORM_POSIX )

namespace efsw { namespace Platform {

class System
{
	public:
		static void sleep( const unsigned long& ms );
};

}}

#endif

#endif
