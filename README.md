Entropia File System Watcher
============================
**efsw** is a C++ cross-platform file system watcher and notifier.

**efsw** monitors the file system asynchronously for changes to files and directories by watching a list of specified paths, and raises events when a directory, or file change.

**efsw** currently supports the following platforms:

* Linux via [inotify](http://en.wikipedia.org/wiki/Inotify)

* Windows via [I/O Completion Ports](http://en.wikipedia.org/wiki/IOCP)

* Mac OS X via [FSEvents](http://en.wikipedia.org/wiki/FSEvents) and [kqueue](http://en.wikipedia.org/wiki/Kqueue)

* FreeBSD/BSD via [kqueue](http://en.wikipedia.org/wiki/Kqueue)

* OS-independent generic watcher
(polling the disk for directory snapshots and comparing them periodically)

Code License
------------
[MIT License](http://www.opensource.org/licenses/mit-license.php)

Some example code:
------------------

    :::c++
        /// Inherits from the abstract listener class, and implements the the file action handler
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

        // Add a folder to watch, and get the watch ID.
        efsw::WatchID watchID = fileWatcher->addWatch( "My_Path", listener );

		// Start watching asynchronously the directories
		fileWatcher.watch();

        // Remove the watcher
        fileWatcher->removeWatch( watchID );