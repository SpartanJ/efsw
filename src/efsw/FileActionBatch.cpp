#include <efsw/FileActionBatch.hpp>

namespace efsw {

FileActionBatch::FileActionBatch() = default;

FileActionBatch::~FileActionBatch() = default;

FileActionBatch::FileActionBatch( FileActionBatch&& ) noexcept = default;

FileActionBatch& FileActionBatch::operator=( FileActionBatch&& ) noexcept = default;

void FileActionBatch::clear() {
	mEvents.clear();
	mCanceled = false;
}

void FileActionBatch::add( WatchID watchid, const std::string& directory,
						   const std::string& filename, Action action,
						   const std::string& oldFilename ) {
	mEvents.push_back( { watchid, action, directory, filename, oldFilename } );
}

void FileActionBatch::dispatch( FileWatchListener* listener ) {
	if ( listener ) {
		for ( size_t i = 0; i < mEvents.size() && !mCanceled; ++i ) {
			const Event& event = mEvents[i];
			listener->handleFileAction( event.Watch, event.Directory, event.Filename,
										event.ActionType, event.OldFilename );
		}
	}
	// Reuse capacity for steady workloads, but release a large burst's storage once a
	// subsequent scan uses less than a quarter of it. clear() alone retains that capacity.
	const bool shrink = mEvents.capacity() > 1024 && mEvents.size() < mEvents.capacity() / 4;
	clear();
	if ( shrink )
		std::vector<Event>().swap( mEvents );
}

void FileActionBatch::cancel() {
	mCanceled = true;
}

} // namespace efsw
