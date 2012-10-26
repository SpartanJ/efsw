/**
	Implementation header file for Linux based on inotify.

	@author James Wynn
	@date 4/15/2009

	Copyright (c) 2009 James Wynn (james@jameswynn.com)

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE.
*/
#ifndef _FW_FILEWATCHERGENERIC_H_
#define _FW_FILEWATCHERGENERIC_H_

#include <efsw/FileWatcherImpl.hpp>
#include <efsw/FileInfo.hpp>
#include <efsw/FileSystem.hpp>
#include <list>
#include <map>

namespace efsw
{
	class DirectoryWatch;

	class WatcherGeneric : public Watcher
	{
		public:
			DirectoryWatch *		DirWatch;
			FileWatcherImpl *		WatcherImpl;
			DirectoryWatch *		CurDirWatch;

			WatcherGeneric( WatchID id, const std::string& directory, FileWatchListener * fwl, FileWatcherImpl * fw, bool recursive );

			~WatcherGeneric();

			void watch();
	};

	class DirectoryWatch
	{
		public:
			typedef std::map<std::string, FileInfo> FileInfoMap;
			typedef std::map<std::string, DirectoryWatch*> DirWatchMap;

			WatcherGeneric *	Watch;
			std::string			Directory;
			FileInfoMap			Files;
			DirWatchMap			Directories;
			FileInfo			DirInfo;
			bool				Recursive;

			DirectoryWatch( WatcherGeneric * ws, const std::string& directory, bool recursive );

			~DirectoryWatch();

			void watch();

			static bool isDir( const std::string& directory );

			void GetFiles( const std::string& directory, FileInfoMap& files );
		protected:
			bool			Deleted;
	};

	/// Implementation for Generic File Watcher.
	/// @class FileWatcherGeneric
	class FileWatcherGeneric : public FileWatcherImpl
	{
	public:
		typedef std::list<WatcherGeneric*> WatchList;

		FileWatcherGeneric();

		virtual ~FileWatcherGeneric();

		/// Add a directory watch
		/// On error returns WatchID with Error type.
		WatchID addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive);

		/// Remove a directory watch. This is a brute force lazy search O(nlogn).
		void removeWatch(const std::string& directory);

		/// Remove a directory watch. This is a map lookup O(logn).
		void removeWatch(WatchID watchid);

		/// Updates the watcher. Must be called often.
		void watch();

		/// Handles the action
		void handleAction(Watcher * watch, const std::string& filename, unsigned long action);

		/// @return Returns a list of the directories that are being watched
		std::list<std::string> directories();
	protected:
		/// Map of WatchID to WatchStruct pointers
		WatchList mWatches;

		/// The last watchid
		WatchID mLastWatchID;

		Thread * mThread;

		Mutex mWatchesLock;
	private:
		void run();
	};
}

#endif
