#ifndef EFSW_FUZZ_REPLAY_HPP
#define EFSW_FUZZ_REPLAY_HPP

#include <cstddef>

namespace efsw_fuzz {

struct Replay {

	/// Unknown registrations are errors, not indices into the option table. A rename inside
	/// one registration does not require cross-directory correlation to be enabled.
	template <size_t Count>
	static bool allowsMove( int source, int destination, const bool ( &opted )[Count] ) {
		return source >= 0 && destination >= 0 && static_cast<size_t>( source ) < Count &&
			   static_cast<size_t>( destination ) < Count &&
			   ( source == destination || ( opted[source] && opted[destination] ) );
	}
};

} // namespace efsw_fuzz

#endif
