#ifndef EFSW_ATOMIC_BOOL_HPP
#define EFSW_ATOMIC_BOOL_HPP

#include <atomic>

namespace efsw {

class AtomicBool
{
	public:
		explicit AtomicBool(bool set = false)
			: set_(set) {}

		AtomicBool& operator= (bool set) {
			set_.store(set, std::memory_order_release);
			return *this;
		}

		explicit operator bool() const {
			return set_.load(std::memory_order_acquire);
		}

	private:
		std::atomic<bool> set_;
};

}

#endif

