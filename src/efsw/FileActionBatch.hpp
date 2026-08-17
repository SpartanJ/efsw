#ifndef EFSW_FILEACTIONBATCH_HPP
#define EFSW_FILEACTIONBATCH_HPP

#include <efsw/FileInfo.hpp>
#include <efsw/efsw.hpp>
#include <string>
#include <vector>

namespace efsw {

class FileActionBatch {
  public:
	void clear();

	void add( WatchID watchid, const std::string& directory, const std::string& filename,
			  Action action, const std::string& oldFilename, const FileInfo& fileInfo );

	void dispatch( FileWatchListener* listener );

  protected:
	struct Event {
		WatchID Watch;
		std::string Directory;
		std::string Filename;
		Action ActionType;
		std::string OldFilename;
		FileInfo Info;

		Event( WatchID watchid, const std::string& directory, const std::string& filename,
			   Action action, const std::string& oldFilename, const FileInfo& fileInfo );
	};

	std::vector<Event> mEvents;
};

} // namespace efsw

#endif
