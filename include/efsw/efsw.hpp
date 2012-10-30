/**
	@author Martín Lucas Golini

	Copyright (c) 2012 Martín Lucas Golini

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

	This software is a fork of the "simplefilewatcher" by James Wynn (james@jameswynn.com)
	http://code.google.com/p/simplefilewatcher/ also MIT licensed.
*/

#ifndef ESFW_HPP
#define ESFW_HPP

#include <string>
#include <list>
#include <iostream>

namespace efsw {

/// Type for a watch id
typedef long WatchID;

// forward declarations
class FileWatcherImpl;
class FileWatchListener;

/// Actions to listen for. Rename will send two events, one for
/// the deletion of the old file, and one for the creation of the
/// new file.
namespace Actions {
	enum Action
	{
		/// Sent when a file is created or renamed
		Add = 1,
		/// Sent when a file is deleted or renamed
		Delete = 2,
		/// Sent when a file is modified
		Modified = 4
	};
}
typedef Actions::Action Action;

/// Errors log namespace
namespace Errors {

enum Error
{
	FileNotFound	= -1,
	Unspecified		= -2
};

class Log
{
	public:
		/// @return The last error logged
		static std::string getLastErrorLog();

		/// Creates an error of the type specified
		static Error createLastError( Error err, std::string log );
};

}

typedef Errors::Error Error;

/// Listens to files and directories and dispatches events
/// to notify the parent program of the changes.
/// @class FileWatcher
class FileWatcher
{
	public:
		/// Default constructor, will use the default platform file watcher
		FileWatcher();

		/// Constructor that lets you force the use of the Generic File Watcher
		FileWatcher( bool useGenericFileWatcher );

		virtual ~FileWatcher();

		/// Add a directory watch. Same as the other addWatch, but doesn't have recursive option.
		/// For backwards compatibility.
		/// On error returns WatchID with Error type.
		WatchID addWatch(const std::string& directory, FileWatchListener* watcher);

		/// Add a directory watch
		/// On error returns WatchID with Error type.
		WatchID addWatch(const std::string& directory, FileWatchListener* watcher, bool recursive);

		/// Remove a directory watch. This is a brute force search O(nlogn).
		void removeWatch(const std::string& directory);

		/// Remove a directory watch. This is a map lookup O(logn).
		void removeWatch(WatchID watchid);

		/// Starts watching ( in other thread )
		void watch();

		/// @return Returns a list of the directories that are being watched
		std::list<std::string> directories();
	private:
		/// The implementation
		FileWatcherImpl* mImpl;
};

/// Basic interface for listening for file events.
/// @class FileWatchListener
class FileWatchListener
{
	public:
		FileWatchListener() {}

		virtual ~FileWatchListener() {}

		/// Handles the action file action
		/// @param watchid The watch id for the directory
		/// @param dir The directory
		/// @param filename The filename that was accessed (not full path)
		/// @param action Action that was performed
		virtual void handleFileAction(WatchID watchid, const std::string& dir, const std::string& filename, Action action) = 0;

};

}

#endif
