Entropia File System Watcher ![efsw](https://web.ensoft.dev/efsw/efsw-logo.svg)
============================

[![build status](https://img.shields.io/github/actions/workflow/status/SpartanJ/efsw/main.yml?branch=master)](https://github.com/SpartanJ/efsw/actions?query=workflow%3Abuild)

**efsw** is a C++ cross-platform file system watcher and notifier.

**efsw** monitors the file system asynchronously for changes to files and directories by watching a list of specified paths, and raises events when a directory or file change.

**efsw** supports recursive directories watch, tracking the entire sub directory tree.

**efsw** currently supports the following platforms:

* Linux via [inotify](http://en.wikipedia.org/wiki/Inotify)

* Windows via [I/O Completion Ports](http://en.wikipedia.org/wiki/IOCP)

* macOS via [FSEvents](http://en.wikipedia.org/wiki/FSEvents) or [kqueue](http://en.wikipedia.org/wiki/Kqueue)

* FreeBSD/BSD via [kqueue](http://en.wikipedia.org/wiki/Kqueue)

* OS-independent generic watcher
(polling the disk for directory snapshots and comparing them periodically)

If any of the backend fails to start for any reason, it will fallback to the OS-independent implementation.
This should never happen, except for the Kqueue implementation; see `Platform limitations and clarifications`.

**Code License**
--------------
[MIT License](http://www.opensource.org/licenses/mit-license.php)

**Some example code:**
--------------------

```c++
// Inherits from the abstract listener class, and implements the the file action handler
class UpdateListener : public efsw::FileWatchListener {
  public:
    void handleFileAction( efsw::WatchID watchid, const std::string& dir,
                           const std::string& filename, efsw::Action action,
                           const std::string& oldFilename ) override {
        switch ( action ) {
            case efsw::Actions::Add:
                std::cout << "DIR (" << dir << ") FILE (" << filename << ") has event Added"
                          << std::endl;
                break;
            case efsw::Actions::Delete:
                std::cout << "DIR (" << dir << ") FILE (" << filename << ") has event Delete"
                          << std::endl;
                break;
            case efsw::Actions::Modified:
                std::cout << "DIR (" << dir << ") FILE (" << filename << ") has event Modified"
                          << std::endl;
                break;
            case efsw::Actions::Moved:
                std::cout << "DIR (" << dir << ") FILE (" << filename << ") has event Moved from ("
                          << oldFilename << ")" << std::endl;
                break;
            default:
                std::cout << "Should never happen!" << std::endl;
        }
    }
};

// Create the file system watcher instance
// efsw::FileWatcher allow a first boolean parameter that indicates if it should start with the
// generic file watcher instead of the platform specific backend
efsw::FileWatcher* fileWatcher = new efsw::FileWatcher();

// Create the instance of your efsw::FileWatcherListener implementation
UpdateListener* listener = new UpdateListener();

// Add a folder to watch, and get the efsw::WatchID
// It will watch the /tmp folder recursively ( the third parameter indicates that is recursive )
// Reporting the files and directories changes to the instance of the listener
efsw::WatchID watchID = fileWatcher->addWatch( "/tmp", listener, true );

// Adds another directory to watch. This time as non-recursive.
efsw::WatchID watchID2 = fileWatcher->addWatch( "/usr", listener, false );

// For Windows, adds another watch, specifying to use a bigger buffer, to not miss events
// (do not use for network locations, see efsw.hpp for details).
efsw::WatchID watchID3 = fileWatcher->addWatch( "c:\\temp", listener, true, { (BufferSize, 128*1024) } );

// Start watching asynchronously the directories
fileWatcher->watch();

// Remove the second watcher added
// You can also call removeWatch by passing the watch path ( it must end with an slash or backslash
// in windows, since that's how internally it's saved )
fileWatcher->removeWatch( watchID2 );
```

**Dependencies**
--------------
None :)

**Compiling**
------------
To generate project files you will need to [download and install](https://premake.github.io/download) [Premake](https://premake.github.io/docs/What-Is-Premake)

Then you can generate the project for your platform by just going to the project directory where the premake4.lua file is located and executing:

`premake5 gmake` to generate project Makefiles, then `cd make/*YOURPLATFORM*/`, and finally `make` or `make config=release_x86_64` ( it will generate the static lib, the shared lib and the test application ).

or

`premake5 vs2022` to generate Visual Studio 2022 project.

or

`premake5 xcode4` to generate Xcode 4 project.

There is also a cmake file that I don't officially support but it works just fine, provided by [Mohammed Nafees](https://github.com/mnafees) and improved by [Eugene Shalygin](https://github.com/zeule).

**Packages**
------------

Community has been kind and contributed the recipes for different popular packages systems: [Conan](https://conan.io/center/recipes/efsw), [vcpkg](https://vcpkg.io/en/package/efsw), [xmake-repo](https://github.com/xmake-io/xmake-repo/blob/dev/packages/e/efsw/xmake.lua). I don't personally maintain those packages so they could be not up to date, but they tend to be quite up to date.

**Platform limitations and clarifications**
-------------------------------------------

### General

* Directory paths are expected as UTF-8 strings on every platform, and `handleFileAction` reports
  UTF-8 strings.
* A file may be reported as `Modified` several times while it is copied, typically after each write
  flush: file watchers cannot avoid it. To open a file after a modification event, wait a
  reasonable time after the last event.
* The Windows and macOS FSEvents backends cannot follow symlinks: they ignore `followSymlinks()`
  and `allowOutOfScopeLinks()`.
* The generic watcher, kqueue, and FSEvents before macOS 10.7, whose events only name the changed
  directories, keep a cache of the watched directory trees to detect changes, which costs memory.
* The generic watcher detects renames and moves by filesystem identity: device and inode on POSIX,
  volume serial number and file index on Windows. Without a stable, unambiguous identity, it
  reports `Delete` and `Add` events instead.

### Linux (inotify)

* Recursive watching uses one inotify watch per directory. `addWatch()` returns
  `Errors::FileOverlapping` for a registration that overlaps an explicit one, when either of them
  recursively covers the other's root. Applications that need several consumers of one subtree
  should register it once and multiplex the events. Registrations can still come to overlap later,
  for example when a directory holding a registration is moved into a recursively watched tree:
  efsw then shares the native watch, delivers its events to every registration covering the
  directory, and removing one of them keeps the others working.

### Windows

* The native backend needs Windows 10 version 1709 or newer and NTFS for the extended file
  identifiers of `ReadDirectoryChangesExW`. Without them it falls back to `ReadDirectoryChangesW`:
  watching works, but cross-directory moves are reported as `Delete` and `Add`. The generic watcher
  does not need `ReadDirectoryChangesExW`, and correlates moves on older Windows versions when the
  filesystem gives stable volume and file identifiers.

### macOS

* All the paths registered on one `FileWatcher` share one FSEvents stream, so each `FileWatcher`
  takes one client from the system-wide FSEvents client pool, not one per `addWatch` call. Paths
  added before `watch()` start together; adding or removing one afterwards starts the replacement
  stream before stopping the previous one, leaving no gap.
* FSEvents on macOS 10.7 and later may report more actions than occurred, since its fine-grained
  events do not give their order: in some cases efsw guesses or approximates it.
* Below macOS 10.5 only kqueue is available, and efsw has to be compiled separately for it, since
  the FSEvents backend cannot be compiled there.

### kqueue (FreeBSD/BSD and macOS)

* kqueue is limited by the number of file descriptors a process may open, around 18000 on BSD and
  10240 on macOS. When it runs out, efsw falls back to the generic watcher.

### Cross-directory moves

Setting `Options::ReportCrossDirectoryMoves` reports a move between directories as one `Moved`
event, whose `oldFilename` is the absolute source path. It works within a recursive watch, and
between separately registered watches of one `FileWatcher` when both enable the option. It is
best-effort, and supported by the inotify, Windows, FSEvents, kqueue and generic backends. Below,
the polling backends are the generic watcher and kqueue.

A move is still reported as `Delete` and/or `Add` when:

* the option is not enabled on both watches;
* it goes into or out of the watched directories;
* its native event pair is incomplete, or its filesystem identity ambiguous;
* on the polling backends, its halves are seen in different polling passes within one recursive
  watch. Between two watches, a move that happens between their scans is still paired;
* registrations are nested and a watch covers only one of its paths: that watch gets its half,
  while the watch covering both paths reports the move.

A directory moved as a whole is reported by its own `Moved` event only: the entries inside it, whose
paths relative to it are unchanged, are not reported. On the polling backends, what changed inside
the moved tree before the next polling pass is reported after that event, by comparing the tree's
snapshots at both ends: entries deleted inside it first, at the paths the directory's `Moved` event
gave them, then entries renamed or relocated inside it as `Moved` events, and new entries as `Add`
events, parents first. This only uses snapshots the watches already keep, for one polling pass.

kqueue only reports that a watched directory changed, not what changed, so efsw rescans each
directory it is told about, and every directory of a tree has its own snapshot. When a directory
moved between watches is changed inside while it arrives, on a heavily loaded system its
directories may be scanned far enough apart that the comparison of its tree misses some of those
changes, or reports part of them again. The changes made after its arrival are reported normally.

To find the other half of a move, events can be delayed. The order of the events of one watch is
kept, but events of watches with the option can be delivered after later events of other watches.

* Polling backends, with the option: `Add` and `Delete` events wait until the end of the polling
  pass. A `Delete` in a watch scanned after another watch with the option waits one more pass, with
  the later events of its watch, including the moves into it, since that other watch may only see
  the destination on its next scan.
* Windows, with the option: `Add` and `Delete` events wait up to 100 ms.
* inotify, with or without the option: the kernel queues the two halves of a rename one after the
  other, so efsw can read the source before its destination is queued. A batch of events holding a
  move's source without its destination waits up to 20 ms for it, counted from the batch's first
  read, and is then delivered in order. A file moved out of the watched directories has no
  destination, so its `Delete`, and the events read with it, can wait up to 20 ms.

**Useful information**
--------------------
The project also comes with a C API wrapper, contributed by [Sepul Sepehr Taghdisian](https://github.com/septag).

There's a string manipulation class not exposed in the efsw header ( efsw::String ) that can be used to make string encoding conversion.

**Clarifications**
----------------

This software started as a fork of the [simplefilewatcher](http://code.google.com/p/simplefilewatcher/) by James Wynn (james[at]jameswynn.com), [MIT licensed](http://www.opensource.org/licenses/mit-license.html).

The icon used for the project is part of the [Haiku®'s Icons](http://www.haiku-inc.org/haiku-icons.html), [MIT licensed](http://www.opensource.org/licenses/mit-license.html).
