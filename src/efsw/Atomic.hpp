#ifndef EFSW_ATOMIC_BOOL_HPP
#define EFSW_ATOMIC_BOOL_HPP

#include <efsw/base.hpp>

#ifdef EFSW_USE_CXX11
#include <atomic>
#endif

namespace efsw {

template <typename T> class Atomic {
  public:
	explicit Atomic( T set = false ) : set_( set ) {}

	Atomic& operator=( T set ) {
#ifdef EFSW_USE_CXX11
		set_.store( set, std::memory_order_release );
#else
		set_ = set;
#endif
		return *this;
	}

	explicit operator T() const {
#ifdef EFSW_USE_CXX11
		return set_.load( std::memory_order_acquire );
#else
		return set_;
#endif
	}

	T load() const {
#ifdef EFSW_USE_CXX11
		return set_.load( std::memory_order_acquire );
#else
		return set_;
#endif
	}

  private:
#ifdef EFSW_USE_CXX11
	std::atomic<T> set_;
#else
	volatile T set_;
#endif
};

} // namespace efsw

#endif
