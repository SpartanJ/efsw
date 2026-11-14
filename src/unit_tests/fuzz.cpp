#include "../fuzz/replay.hpp"
#include "utest.h"

UTEST( FuzzReplay, UnknownRegistrationsAreNotOptionIndices ) {
	const bool opted[]{ true, true, false };
	EXPECT_FALSE( efsw_fuzz::Replay::allowsMove( 0, -1, opted ) );
	EXPECT_FALSE( efsw_fuzz::Replay::allowsMove( -1, 0, opted ) );
	EXPECT_FALSE( efsw_fuzz::Replay::allowsMove( -1, -1, opted ) );
	EXPECT_FALSE( efsw_fuzz::Replay::allowsMove( 0, 3, opted ) );
	EXPECT_FALSE( efsw_fuzz::Replay::allowsMove( 3, 0, opted ) );
	EXPECT_TRUE( efsw_fuzz::Replay::allowsMove( 0, 1, opted ) );
	EXPECT_FALSE( efsw_fuzz::Replay::allowsMove( 0, 2, opted ) );
	EXPECT_TRUE( efsw_fuzz::Replay::allowsMove( 2, 2, opted ) );
}
