#include "test_util.hpp"
#include "utest.h"
#include <string.h>

namespace efsw_test {
bool useGeneric = false;
}

UTEST_STATE();

int main( int argc, const char* const argv[] ) {
	for ( int i = 1; i < argc; ++i ) {
		if ( strcmp( argv[i], "--generic" ) == 0 ) {
			efsw_test::useGeneric = true;
			break;
		}
	}
	return utest_main( argc, argv );
}
