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
	std::cout << "Press ^C to exit demo" << std::endl;

	bool commonTest = true;
	bool useGeneric = false;
	std::string path;

	if ( argc >= 2 )
	{
		path = std::string( argv[1] );

		if ( efsw::FileSystem::isDirectory( path ) )
		{
			commonTest	= false;
		}

		if ( argc >= 3 )
		{
			if ( std::string( argv[2] ) == "true" )
			{
				useGeneric = true;
			}
		}
	}

	UpdateListener * ul = new UpdateListener();

	/// create the file watcher object
	efsw::FileWatcher fileWatcher( useGeneric );

	if ( commonTest )
	{
		std::string CurPath( FileRemoveFileName( std::string( argv[0] ) ) );

		/// add a watch to the system
		efsw::WatchID watchID = fileWatcher.addWatch( CurPath + "test", ul, true );

		/// starts watching
		fileWatcher.watch();

		/// adds another watch after started watching...
		efsw::System::sleep( 1000 );

		efsw::WatchID watchID2 = fileWatcher.addWatch( CurPath + "test2", ul, true );

		//efsw::System::sleep( 1000 );
		//fileWatcher.removeWatch( watchID );
		//efsw::System::sleep( 1000 );
		//fileWatcher.removeWatch( watchID2 );
	}
	else
	{
		efsw::WatchID err;

		if ( ( err = fileWatcher.addWatch( path, ul, true ) ) > 0 )
		{
			fileWatcher.watch();

			std::cout << "Watching directory: " << path.c_str() << std::endl;

			if ( useGeneric )
			{
				std::cout << "Using generic backend watcher" << std::endl;
			}
		}
		else
		{
			std::cout << "Error trying to watch directory: " << path.c_str() << std::endl;
			std::cout << efsw::Errors::Log::getLastErrorLog().c_str() << std::endl;
		}
	}

	while( true )
	{
		efsw::System::sleep( 1000 );
	}

	return 0;
}
