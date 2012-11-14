#ifndef EFSW_DIRECTORYSNAPSHOTDIFF_HPP
#define EFSW_DIRECTORYSNAPSHOTDIFF_HPP

#include <efsw/FileInfo.hpp>

namespace efsw {

class DirectorySnapshotDiff
{
	public:
		FileInfoList	FilesDeleted;
		FileInfoList	FilesCreated;
		FileInfoList	FilesModified;
		MovedList		FilesMoved;
		FileInfoList	DirsDeleted;
		FileInfoList	DirsCreated;
		FileInfoList	DirsModified;
		MovedList		DirsMoved;

		void clear();

		bool changed();
};

}

#endif

