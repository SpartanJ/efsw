#ifndef EFSW_ATOMIC_BOOL_HPP
#define EFSW_ATOMIC_BOOL_HPP

#include <efsw/base.hpp>

#ifdef EFSW_USE_CXX11
#include <atomic>
#endif

namespace efsw {

class AtomicBool
{
	public:
		explicit AtomicBool(bool set = false)
			: set_(set) {}

		AtomicBool& operator= (bool set) {
#ifdef EFSW_USE_CXX11
			set_.store(set, std::memory_order_release);
#else
			set_ = set;
#endif
			return *this;
		}

		explicit operator bool() const {
#ifdef EFSW_USE_CXX11
			return set_.load(std::memory_order_acquire);
#else
			return set_;
#endif
		}

	private:
#ifdef EFSW_USE_CXX11
		std::atomic<bool> set_;
#else
		volatile bool set_;
#endif
};

}

#endif

