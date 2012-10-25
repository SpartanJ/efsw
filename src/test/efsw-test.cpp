#include <cstdio>
#include <efsw/efsw.hpp>
#include <efsw/System.hpp>
#include <efsw/FileSystem.hpp>

/// Processes a file action
class UpdateListener : public efsw::FileWatchListener
{
	public:
		UpdateListener() {}

		std::string getActionName( efsw::Action action )
		{
			switch ( action )
			{
				case efsw::Actions::Add:		return "Add";
				case efsw::Actions::Modified:	return "Modified";
				case efsw::Actions::Delete:		return "Delete";
				default:						return "Unknown";
			}
		}

		void handleFileAction( efsw::WatchID watchid, const std::string& dir, const std::string& filename, efsw::Action action )
		{
			std::cout << "DIR (" << dir + ") FILE (" + filename + ") has event " << getActionName( action ) << std::endl;
		}
};

std::string FileRemoveFileName( const std::string& filepath ) {
    return filepath.substr( 0, filepath.find_last_of("/\\") + 1 );
}

int main(int argc, char **argv)
{
	UpdateListener * ul = new UpdateListener();

	// create the file watcher object
	efsw::FileWatcher fileWatcher;

	// add a watch to the system
	std::string path( FileRemoveFileName( std::string( *argv ) ) + "test" );

	efsw::WatchID watchID = fileWatcher.addWatch( path, ul, true );

	std::cout << "Press ^C to exit demo" << std::endl;

	// starts watching
	fileWatcher.watch();

	// adds another watch after started watching...
	//efsw::WatchID watchID2 = fileWatcher.addWatch( "./test2", ul, true );
	//efsw::System::sleep( 1000 );
	//fileWatcher.removeWatch( watchID );
	//fileWatcher.removeWatch( "./test2" );

	int count = 0;

	while( count < 1000 )
	{
		efsw::System::sleep( 100 );

		//count+= 100;
	}

	return 0;
}
