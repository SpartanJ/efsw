#ifndef EFSW_RETIRINGWATCHES_HPP
#define EFSW_RETIRINGWATCHES_HPP

#include <efsw/Watcher.hpp>
#include <vector>

namespace efsw {

/// The watches of a polling file watcher, as one pass scans them. A listener may remove a
/// watch from a callback of the pass, while that watch is still on the polling thread's stack:
/// it is retired, which stops its callbacks, and deleted once the pass ends. The pass scans a
/// copy of the watch list, which a removal does not invalidate.
class RetiringWatches {
  public:
	~RetiringWatches() { deleteRetired(); }

	/// Starts a pass. @return The list to fill with the watches to scan, reused between passes.
	std::vector<Watcher*>& beginPass() {
		mInPass = true;
		mScanned.clear();
		return mScanned;
	}

	void endPass() {
		mInPass = false;
		deleteRetired();
	}

	/// Deletes a watch that was removed from the watch list, or retires it during a pass.
	void release( Watcher* watch ) {
		if ( mInPass ) {
			watch->retire();
			mRetired.push_back( watch );
		} else {
			delete watch;
		}
	}

  private:
	std::vector<Watcher*> mScanned;
	std::vector<Watcher*> mRetired;
	bool mInPass{ false };

	void deleteRetired() {
		for ( Watcher* watch : mRetired )
			delete watch;
		mRetired.clear();
	}
};

} // namespace efsw

#endif
