// efsw-fuzz: a real file system fuzzer, with a replay oracle, for the events efsw delivers.
//
// It makes random changes to real directories watched by efsw, then applies the events it was
// told about, in the order it was told, to a model of the watched trees. The model must end
// equal to the real trees. A listener that can not rebuild the tree from the events was lied to:
// an event came in an order it can not apply, named a path that did not exist, or was missing.
//
// This is what found most of the ordering bugs of Options::ReportCrossDirectoryMoves on the
// polling backends: see README.md next to this file for how to run it, read its output, and
// compare two builds.
//
// Layout, in a new directory under /tmp:
//   w0 .. w3  four recursive registrations. w0, w1 and w2 opt in to ReportCrossDirectoryMoves,
//             w3 does not, unless --optall.
//   out       not watched: entries moved there leave the watches, and may come back.
//   With --nested, the root itself is also registered, not recursively, with the option: the
//   w* directories are then nested registrations, and entries can move to and from the root.
//
// Each round makes 1 to --maxops random operations, each one of: create a file, make a
// directory, rename an entry (to any directory, sometimes to a new name), remove a tree, append
// to a file, or sleep part of a polling interval so that the operations spread over passes. It
// then waits for the events, applies them to the model, and compares the model with the trees.
//
// Checks, each reported as an error of the round:
//   - an Add of a path the model has, a Delete or Moved from a path it lacks, a Moved onto a path
//     it has;
//   - a Moved between registrations that did not both opt in;
//   - an event of one registration with a path outside it;
//   - the model and the real trees differ after the round ("missing from replay": a real entry
//     no event reported; "extra in replay": an entry the events left, which is gone).
//
// Two conventions of the polling backends are accepted, unless --strict: a directory's Delete
// may come before those of its contents, and a directory reported as an Add, or moved in from
// where nothing reported its contents, takes them in without events.
//
// A seed fixes the operations, not the timing: whether a seed fails can depend on how the
// operations fall into polling passes or inotify reads, and so on the load of the machine.
//
// POSIX only. Usage: efsw-fuzz --help
#include <efsw/efsw.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <climits>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct Event {
	efsw::WatchID id;
	std::string dir, file, old;
	efsw::Action action;
};

std::mutex gMutex;
std::vector<Event> gEvents;

/// Collects every event, from efsw's threads, for the round to replay.
struct Listener : efsw::FileWatchListener {
	void handleFileAction( efsw::WatchID id, const std::string& dir, const std::string& file,
						   efsw::Action action, const std::string& old ) override {
		std::lock_guard<std::mutex> lock( gMutex );
		gEvents.push_back( { id, dir, file, old, action } );
	}
};

std::string join( std::string dir, const std::string& file ) {
	if ( dir.empty() || dir.back() != '/' )
		dir += '/';
	return dir + file;
}

/// Adds the paths below abs, relative to it with a leading "/" prefixed by rel.
void listTree( const std::string& abs, const std::string& rel, std::set<std::string>& out ) {
	DIR* dir = opendir( abs.c_str() );
	if ( !dir )
		return;
	while ( dirent* entry = readdir( dir ) ) {
		std::string name( entry->d_name );
		if ( name == "." || name == ".." )
			continue;
		std::string path = rel + "/" + name;
		out.insert( path );
		struct stat st;
		if ( lstat( ( abs + "/" + name ).c_str(), &st ) == 0 && S_ISDIR( st.st_mode ) )
			listTree( abs + "/" + name, path, out );
	}
	closedir( dir );
}

void removeTree( const std::string& path ) {
	struct stat st;
	if ( lstat( path.c_str(), &st ) != 0 )
		return;
	if ( S_ISDIR( st.st_mode ) ) {
		std::set<std::string> children;
		DIR* dir = opendir( path.c_str() );
		if ( dir ) {
			while ( dirent* entry = readdir( dir ) ) {
				std::string name( entry->d_name );
				if ( name != "." && name != ".." )
					children.insert( path + "/" + name );
			}
			closedir( dir );
		}
		for ( const std::string& child : children )
			removeTree( child );
		rmdir( path.c_str() );
	} else {
		unlink( path.c_str() );
	}
}

const char* actionName( efsw::Action action ) {
	switch ( action ) {
		case efsw::Actions::Add:
			return "Add";
		case efsw::Actions::Delete:
			return "Delete";
		case efsw::Actions::Modified:
			return "Modified";
		case efsw::Actions::Moved:
			return "Moved";
	}
	return "?";
}

void usage( const char* program ) {
	printf( "Usage: %s [seed] [rounds] [options]\n"
			"\n"
			"Makes random changes to watched directories and replays the events efsw reports\n"
			"against them. Prints each failing round, then a summary line:\n"
			"  seed <seed>: <failing> failing rounds of <rounds>\n"
			"Stops after 3 failing rounds. Exits with 0 when no round failed, 1 when one did, and\n"
			"2 on a usage or setup error.\n"
			"\n"
			"  seed            the random seed, which fixes the operations (default 1)\n"
			"  rounds          the number of rounds (default 20)\n"
			"  --generic       use the generic (polling) backend instead of the native one\n"
			"  --ms N          the generic backend's polling interval, in ms (default 100)\n"
			"  --maxops N      at most N operations per round (default 8)\n"
			"  --optall        w3 opts in to ReportCrossDirectoryMoves too\n"
			"  --nested        also register the root, so that w0..w3 are nested registrations\n"
			"  --strict        do not accept the polling backends' conventions (see the source)\n"
			"  --verbose       also print the rounds that pass\n"
			"  --help          print this\n"
			"\n"
			"See src/fuzz/README.md.\n",
			program );
}

bool parseNumber( const char* text, int& value ) {
	char* end = NULL;
	long parsed = strtol( text, &end, 10 );
	if ( end == text || *end != '\0' || parsed < 0 || parsed > 1000000000L )
		return false;
	value = (int)parsed;
	return true;
}

void printRound( const std::vector<std::string>& operations, const std::vector<Event>& events,
				 std::map<efsw::WatchID, int>& registrationOf, bool withModified ) {
	for ( const std::string& operation : operations )
		printf( "  %s\n", operation.c_str() );
	if ( withModified )
		printf( "-- events:\n" );
	for ( const Event& event : events )
		if ( withModified || event.action != efsw::Actions::Modified )
			printf( "  [w%d] %s %s%s%s\n",
					registrationOf.count( event.id ) ? registrationOf[event.id] : -1,
					actionName( event.action ), join( event.dir, event.file ).c_str(),
					event.old.empty() ? "" : " from ", event.old.c_str() );
}

} // namespace

int main( int argc, char** argv ) {
	int seedValue = 1;
	int rounds = 20;
	bool generic = false;
	int ms = 100, maxops = 8;
	bool optAll = false;
	bool lenient = true;
	bool nested = false;
	bool verbose = false;
	int positional = 0;
	for ( int i = 1; i < argc; i++ ) {
		const bool hasValue = i + 1 < argc;
		if ( !strcmp( argv[i], "--help" ) || !strcmp( argv[i], "-h" ) ) {
			usage( argv[0] );
			return 0;
		} else if ( !strcmp( argv[i], "--generic" ) ) {
			generic = true;
		} else if ( !strcmp( argv[i], "--ms" ) && hasValue && parseNumber( argv[i + 1], ms ) &&
					ms > 0 ) {
			i++;
		} else if ( !strcmp( argv[i], "--maxops" ) && hasValue &&
					parseNumber( argv[i + 1], maxops ) && maxops > 0 ) {
			i++;
		} else if ( !strcmp( argv[i], "--strict" ) ) {
			lenient = false;
		} else if ( !strcmp( argv[i], "--verbose" ) ) {
			verbose = true;
		} else if ( !strcmp( argv[i], "--nested" ) ) {
			nested = true;
		} else if ( !strcmp( argv[i], "--optall" ) ) {
			optAll = true;
		} else if ( argv[i][0] != '-' && positional == 0 && parseNumber( argv[i], seedValue ) ) {
			positional++;
		} else if ( argv[i][0] != '-' && positional == 1 && parseNumber( argv[i], rounds ) ) {
			positional++;
		} else {
			fprintf( stderr, "%s: invalid argument '%s'\n\n", argv[0], argv[i] );
			usage( argv[0] );
			return 2;
		}
	}
	const unsigned seed = (unsigned)seedValue;

	char tmpl[] = "/tmp/efsw-fuzz-XXXXXX";
	if ( !mkdtemp( tmpl ) ) {
		perror( "mkdtemp" );
		return 2;
	}
	// Resolved: efsw reports real paths, and /tmp can be a symbolic link, as on macOS.
	char resolved[PATH_MAX];
	if ( !realpath( tmpl, resolved ) ) {
		perror( "realpath" );
		return 2;
	}
	std::string root = resolved;
	const int W = 4;
	std::vector<std::string> wroot;
	for ( int i = 0; i < W; i++ ) {
		wroot.push_back( root + "/w" + std::to_string( i ) );
		mkdir( wroot.back().c_str(), 0755 );
	}
	mkdir( ( root + "/out" ).c_str(), 0755 );

	// Every random draw below is part of what a seed reproduces: keep their number and order.
	std::mt19937 rng( seed );
	auto rnd = [&]( int n ) { return (int)( rng() % n ); };
	// Pre-populate a little.
	for ( int i = 0; i < W; i++ ) {
		mkdir( ( wroot[i] + "/d" ).c_str(), 0755 );
		close( open( ( wroot[i] + "/d/f" ).c_str(), O_CREAT | O_WRONLY, 0644 ) );
		close( open( ( wroot[i] + "/g" ).c_str(), O_CREAT | O_WRONLY, 0644 ) );
	}

	efsw::FileWatcher fileWatcher( generic, ms );
	Listener listener;
	// The w* index of each registration, W for the root.
	std::map<efsw::WatchID, int> registrationOf;
	bool opted[W + 1];
	opted[W] = true;
	if ( nested ) {
		std::vector<efsw::WatcherOption> options{
			efsw::WatcherOption( efsw::Options::ReportCrossDirectoryMoves, 1 ) };
		efsw::WatchID topId = fileWatcher.addWatch( root, &listener, false, options );
		if ( topId < 0 ) {
			fprintf( stderr, "addWatch of the root failed: %ld\n", (long)topId );
			removeTree( root );
			return 2;
		}
		registrationOf[topId] = W;
	}
	for ( int i = 0; i < W; i++ ) {
		opted[i] = optAll || i < 3;
		std::vector<efsw::WatcherOption> options;
		if ( opted[i] )
			options.push_back( efsw::WatcherOption( efsw::Options::ReportCrossDirectoryMoves, 1 ) );
		efsw::WatchID id = fileWatcher.addWatch( wroot[i], &listener, true, options );
		if ( id < 0 ) {
			fprintf( stderr, "addWatch of %s failed: %ld\n", wroot[i].c_str(), (long)id );
			removeTree( root );
			return 2;
		}
		registrationOf[id] = i;
	}
	fileWatcher.watch();
	std::this_thread::sleep_for( std::chrono::milliseconds( generic ? 3 * ms : 100 ) );

	// What the events say is there: absolute paths under the watched directories.
	std::set<std::string> model;
	auto snapshot = [&]() {
		std::set<std::string> paths;
		for ( int i = 0; i < W; i++ ) {
			std::set<std::string> relative;
			listTree( wroot[i], "", relative );
			for ( const std::string& path : relative )
				paths.insert( wroot[i] + path );
		}
		if ( nested ) {
			DIR* dir = opendir( root.c_str() );
			while ( dirent* entry = readdir( dir ) ) {
				std::string name( entry->d_name );
				if ( name != "." && name != ".." )
					paths.insert( root + "/" + name );
			}
			closedir( dir );
		}
		return paths;
	};
	model = snapshot();
	// The registration a path belongs to: its w* index, W for an entry of the root, or -1.
	auto registrationOfPath = [&]( const std::string& path ) {
		if ( nested && path.compare( 0, root.size() + 1, root + "/" ) == 0 &&
			 path.find( '/', root.size() + 1 ) == std::string::npos )
			return W;
		for ( int i = 0; i < W; i++ )
			if ( path.compare( 0, wroot[i].size() + 1, wroot[i] + "/" ) == 0 )
				return i;
		return -1;
	};

	int failures = 0;
	int nameCounter = 0;
	for ( int round = 0; round < rounds; round++ ) {
		std::vector<std::string> operations;
		int count = 1 + rnd( maxops );
		for ( int op = 0; op < count; op++ ) {
			// The candidates: every entry under w* and out, and the directories among them.
			std::set<std::string> relative;
			std::vector<std::string> all, dirs;
			for ( int i = 0; i < W; i++ ) {
				dirs.push_back( wroot[i] );
				relative.clear();
				listTree( wroot[i], "", relative );
				for ( const std::string& path : relative )
					all.push_back( wroot[i] + path );
			}
			dirs.push_back( root + "/out" );
			if ( nested ) {
				dirs.push_back( root );
				DIR* dir = opendir( root.c_str() );
				while ( dirent* entry = readdir( dir ) ) {
					std::string name( entry->d_name );
					if ( name == "." || name == ".." || name == "out" ||
						 ( name.size() == 2 && name[0] == 'w' ) )
						continue;
					all.push_back( root + "/" + name );
					std::set<std::string> below;
					listTree( root + "/" + name, "", below );
					for ( const std::string& path : below )
						all.push_back( root + "/" + name + path );
				}
				closedir( dir );
			}
			relative.clear();
			listTree( root + "/out", "", relative );
			for ( const std::string& path : relative )
				all.push_back( root + "/out" + path );
			for ( const std::string& path : all ) {
				struct stat st;
				if ( lstat( path.c_str(), &st ) == 0 && S_ISDIR( st.st_mode ) )
					dirs.push_back( path );
			}
			int kind = rnd( 10 );
			std::string name = "n" + std::to_string( nameCounter++ );
			char line[1024];
			if ( kind == 0 ) {
				std::string path = dirs[rnd( dirs.size() )] + "/" + name;
				close( open( path.c_str(), O_CREAT | O_WRONLY, 0644 ) );
				snprintf( line, sizeof line, "create %s", path.c_str() );
			} else if ( kind == 1 ) {
				std::string path = dirs[rnd( dirs.size() )] + "/" + name;
				mkdir( path.c_str(), 0755 );
				snprintf( line, sizeof line, "mkdir %s", path.c_str() );
			} else if ( kind <= 6 && !all.empty() ) {
				std::string source = all[rnd( all.size() )];
				std::string into = dirs[rnd( dirs.size() )];
				// Not into itself; drawn again.
				if ( into.compare( 0, source.size() + 1, source + "/" ) == 0 || into == source ) {
					op--;
					continue;
				}
				std::string destination =
					into + "/" +
					( rnd( 3 ) == 0 ? name : source.substr( source.rfind( '/' ) + 1 ) );
				struct stat st;
				if ( lstat( destination.c_str(), &st ) == 0 ) {
					op--;
					continue;
				}
				int result = rename( source.c_str(), destination.c_str() );
				snprintf( line, sizeof line, "rename %s -> %s (%d)", source.c_str(),
						  destination.c_str(), result );
			} else if ( kind == 7 && !all.empty() ) {
				std::string path = all[rnd( all.size() )];
				removeTree( path );
				snprintf( line, sizeof line, "rm -rf %s", path.c_str() );
			} else if ( kind == 8 && !all.empty() ) {
				std::string path = all[rnd( all.size() )];
				struct stat st;
				if ( lstat( path.c_str(), &st ) == 0 && S_ISREG( st.st_mode ) ) {
					int fd = open( path.c_str(), O_WRONLY | O_APPEND );
					if ( fd >= 0 ) {
						if ( write( fd, "x", 1 ) < 0 ) {
						}
						close( fd );
					}
					snprintf( line, sizeof line, "write %s", path.c_str() );
				} else {
					op--;
					continue;
				}
			} else {
				// Sleep a random part of a pass to split operations across passes.
				int pause = rnd( generic ? ms : 30 );
				std::this_thread::sleep_for( std::chrono::milliseconds( pause ) );
				snprintf( line, sizeof line, "sleep %d", pause );
			}
			operations.push_back( line );
		}
		// Waits for the events: up to two more polling passes and a margin, or for inotify's
		// batches. Under heavy load events can arrive later, and are then reported by the next
		// round, which fails too.
		std::this_thread::sleep_for( std::chrono::milliseconds( generic ? 5 * ms : 150 ) );
		std::vector<Event> events;
		{
			std::lock_guard<std::mutex> lock( gMutex );
			events.swap( gEvents );
		}
		std::vector<std::string> errors;
		// The conventions of the polling backends, accepted unless --strict: a directory's Delete
		// may come before its children's, and a directory reported as Add takes in its contents
		// silently.
		std::set<std::string> deletedDirs, addedDirs;
		auto under = []( const std::string& path, const std::set<std::string>& roots ) {
			for ( const std::string& directory : roots )
				if ( path.compare( 0, directory.size() + 1, directory + "/" ) == 0 )
					return true;
			return false;
		};
		for ( const Event& event : events ) {
			int registration = registrationOf.count( event.id ) ? registrationOf[event.id] : -1;
			std::string path = join( event.dir, event.file );
			if ( registration < 0 || registrationOfPath( path ) != registration )
				errors.push_back( "event outside its registration: " + path );
			if ( event.action == efsw::Actions::Add ) {
				if ( model.count( path ) )
					errors.push_back( "Add of existing " + path );
				model.insert( path );
				addedDirs.insert( path );
			} else if ( event.action == efsw::Actions::Delete ) {
				if ( !model.count( path ) &&
					 !( lenient && ( under( path, deletedDirs ) || under( path, addedDirs ) ) ) )
					errors.push_back( "Delete of missing " + path );
				deletedDirs.insert( path );
				auto it = model.lower_bound( path );
				while ( it != model.end() &&
						( *it == path || it->compare( 0, path.size() + 1, path + "/" ) == 0 ) )
					it = model.erase( it );
			} else if ( event.action == efsw::Actions::Moved ) {
				// Absolute between registrations, relative to the directory within one.
				std::string old = ( !event.old.empty() && event.old[0] == '/' )
									  ? event.old
									  : join( event.dir, event.old );
				int oldRegistration = registrationOfPath( old );
				if ( oldRegistration != registration &&
					 !( oldRegistration >= 0 && opted[oldRegistration] && opted[registration] ) )
					errors.push_back( "Moved across non-opted registrations: " + old + " -> " +
									  path );
				if ( !model.count( old ) && !( lenient && under( old, addedDirs ) ) )
					errors.push_back( "Moved from missing " + old + " -> " + path );
				if ( model.count( path ) )
					errors.push_back( "Moved onto existing " + old + " -> " + path );
				std::vector<std::string> moved;
				auto it = model.lower_bound( old );
				while ( it != model.end() &&
						( *it == old || it->compare( 0, old.size() + 1, old + "/" ) == 0 ) ) {
					moved.push_back( path + it->substr( old.size() ) );
					it = model.erase( it );
				}
				if ( moved.empty() || addedDirs.count( old ) || under( old, addedDirs ) )
					addedDirs.insert( path );
				// Directories taken in silently below the moved one move with it.
				{
					std::vector<std::string> rebased;
					for ( auto added = addedDirs.begin(); added != addedDirs.end(); ) {
						if ( added->compare( 0, old.size() + 1, old + "/" ) == 0 ) {
							rebased.push_back( path + added->substr( old.size() ) );
							added = addedDirs.erase( added );
						} else {
							++added;
						}
					}
					addedDirs.insert( rebased.begin(), rebased.end() );
				}
				if ( std::find( moved.begin(), moved.end(), path ) == moved.end() )
					moved.push_back( path );
				for ( const std::string& entry : moved )
					if ( registrationOfPath( entry ) >= 0 )
						model.insert( entry );
				// Moved in from the root, which is not watched recursively: its contents were
				// not reported.
				if ( oldRegistration == W )
					addedDirs.insert( path );
			}
		}
		std::set<std::string> real = snapshot();
		for ( const std::string& path : real )
			if ( !model.count( path ) ) {
				if ( lenient && under( path, addedDirs ) )
					model.insert( path );
				else
					errors.push_back( "missing from replay: " + path );
			}
		for ( const std::string& path : model )
			if ( !real.count( path ) )
				errors.push_back( "extra in replay: " + path );
		if ( verbose && errors.empty() ) {
			printf( "--- seed %u round %d ok\n", seed, round );
			printRound( operations, events, registrationOf, false );
		}
		if ( !errors.empty() ) {
			failures++;
			printf( "=== seed %u round %d FAILED\n-- ops:\n", seed, round );
			printRound( operations, events, registrationOf, true );
			printf( "-- errors:\n" );
			for ( const std::string& error : errors )
				printf( "  %s\n", error.c_str() );
			fflush( stdout );
			// Resynchronized: the next round starts from the real trees.
			model = real;
			if ( failures >= 3 )
				break;
		}
	}
	printf( "seed %u: %d failing rounds of %d\n", seed, failures, rounds );
	removeTree( root );
	return failures ? 1 : 0;
}
