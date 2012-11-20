Entropia File System Watcher
============================
**efsw** is a C++ cross-platform file system watcher and notifier.

**efsw** monitors the file system asynchronously for changes to files and directories by watching a list of specified paths, and raises events when a directory or file change.

**efsw** supports recursive directories watch, tracking the entire sub directory tree.

**efsw** currently supports the following platforms:

* Linux via [inotify](http://en.wikipedia.org/wiki/Inotify)

* Windows via [I/O Completion Ports](http://en.wikipedia.org/wiki/IOCP)

* Mac OS X via [FSEvents](http://en.wikipedia.org/wiki/FSEvents) and [kqueue](http://en.wikipedia.org/wiki/Kqueue)

* FreeBSD/BSD via [kqueue](http://en.wikipedia.org/wiki/Kqueue)

* OS-independent generic watcher
(polling the disk for directory snapshots and comparing them periodically)

**Code License**
--------------
[MIT License](http://www.opensource.org/licenses/mit-license.php)

**Some example code:**
--------------------

    :::c++
	// Inherits from the abstract listener class, and implements the the file action handler
	class UpdateListener : public efsw::FileWatchListener
	{
	public:
		UpdateListener() {}

		void handleFileAction( efsw::WatchID watchid, const std::string& dir, const std::string& filename, efsw::Action action, std::string oldFilename = "" )
		{
			switch( action )
			{
			case efsw::Actions::Add:
				std::cout << "DIR (" << dir << ") FILE (" << filename << ") has event Added" << std::endl;
				break;
			case efsw::Actions::Delete:
				std::cout << "DIR (" << dir << ") FILE (" << filename << ") has event Delete" << std::endl;
				break;
			case efsw::Actions::Modified:
				std::cout << "DIR (" << dir << ") FILE (" << filename << ") has event Modified" << std::endl;
				break;
			case efsw::Actions::Moved:
					std::cout << "DIR (" << dir << ") FILE (" << filename << ") has event Moved from (" << oldFilename << ")" << std::endl;
				break;
			default:
				std::cout << "Should never happen!" << std::endl;
			}
		}
	};

	// Create the file system watcher instance
	efsw::FileWatcher * fileWatcher = new efsw::FileWatcher();

	// Create the instance of your efsw::FileWatcherListener implementation
	UpdateListener * listener = new UpdateListener();

	// Add a folder to watch, and get the efsw::WatchID
	// It will watch the /tmp folder recursively ( the third parameter indicates that is recursive )
	// Reporting the files and directories changes to the instance of the listener
	efsw::WatchID watchID = fileWatcher->addWatch( "/tmp", listener, true );

	// Start watching asynchronously the directories
	fileWatcher.watch();

	// Remove the watcher
	fileWatcher->removeWatch( watchID );
	

**Dependencies**
--------------
None :)

**Compiling**
------------
To generate project files you will need to [download and install](http://industriousone.com/premake/download) [Premake](http://industriousone.com/what-premake)

Then you can generate the project for your platform just going to the project directory where the premake4.lua file is located and then execute:

`premake4 gmake` to generate project Makefiles, then `cd make/*YOURPLATFORM*/`, and finally `make` or `make config=release` ( it will generate the static lib, the shared lib and the test application ).

or 

`premake4 vs2010` to generate Visual Studio 2010 project.

or

`premake4 xcode4` to generate Xcode 4 project.


**Platform Limitations**
---------------------

Windows and FSEvents Mac OS X implementation can't follow symlinks.

Kqueue implementation is limited by the maximun number of file descriptors allowed per process by the OS, in the case of reaching the file descriptors limit ( in BSD around 18000 and in OS X around 10240 ) it will fallback to the generic file watcher.

FSEvents for OS X Lion and beyond in some cases will generate more actions that in reality ocurred, since fine-grained implementation of FSEvents doesn't give the order of the actions retrieved, in some cases i need to guess/aproximate the order of them.

Generic watcher relies on the inode information to detect file and directories renames/move. Since Windows has no concept of inodes as Unix-y platforms do, there is no current reliable way of determining file/directory movement on Windows without help from the Windows API.

**Clarifications**
----------------

This software started as a fork of the [simplefilewatcher](http://code.google.com/p/simplefilewatcher/) by James Wynn (james[at]jameswynn.com), [MIT licensed](http://www.opensource.org/licenses/mit-license.html).

The icon used for the project is part of the [Haiku®'s Icons](http://www.haiku-inc.org/haiku-icons.html), [MIT licensed](http://www.opensource.org/licenses/mit-license.html).