#ifndef EFSW_FILEACTIONBATCH_HPP
#define EFSW_FILEACTIONBATCH_HPP

#include <efsw/efsw.hpp>
#include <string>
#include <vector>

namespace efsw {

/// A recursive polling scan finishes updating its snapshots before calling listeners.
/// This queue preserves the collected order; it does not correlate moves.
class FileActionBatch {
  public:
	FileActionBatch();

	~FileActionBatch();

	FileActionBatch( FileActionBatch&& ) noexcept;

	FileActionBatch& operator=( FileActionBatch&& ) noexcept;

	void clear();

	void add( WatchID watchid, const std::string& directory, const std::string& filename,
			  Action action, const std::string& oldFilename );

	void dispatch( FileWatchListener* listener );

	/// Stops dispatch when a callback removes its registration. Does not invalidate the
	/// strings passed to the callback currently running.
	void cancel();

  protected:
	struct Event {
		WatchID Watch;
		Action ActionType;
		std::string Directory;
		std::string Filename;
		std::string OldFilename;
	};

	std::vector<Event> mEvents;
	bool mCanceled{ false };
};

} // namespace efsw

#endif
