#ifndef EFSW_DEBUG_HPP
#define EFSW_DEBUG_HPP

#include <efsw/base.hpp>

namespace efsw {

#ifdef DEBUG

void efREPORT_ASSERT( const char * File, const int Line, const char * Exp );

#define efASSERT( expr )		if ( !(expr) ) { efREPORT_ASSERT( __FILE__, __LINE__, #expr	);	}
#define efASSERTM( expr, msg )	if ( !(expr) ) { efREPORT_ASSERT( __FILE__, __LINE__, #msg	);	}

void efPRINT 	( const char * format, ... );
void efPRINTC	( unsigned int cond, const char * format, ... );

#else

#define efASSERT( expr )
#define efASSERTM( expr, msg )

#define efPRINT( format, args... ) {}
#define efPRINTC( cond, format, args... ) {}

#endif

#ifdef EFSW_VERBOSE
	#define efDEBUG efPRINT
	#define efDEBUGC efPRINTC
#else
	#define efDEBUG( format, args... ) {}
	#define efDEBUGC( cond, format, args... ) {}
#endif

}

#endif
