/**
	Demo app for FileWatcher. FileWatcher is a simple wrapper for the file
	modification system in Windows and Linux.

	@author James Wynn
	@date 2/25/2009

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

#include <cstdio>
#include <efsw/efsw.hpp>
#include <efsw/System.hpp>

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

int main(int argc, char **argv)
{
	try 
	{
		UpdateListener * ul = new UpdateListener();

		// create the file watcher object
		efsw::FileWatcher fileWatcher(true);

		// add a watch to the system
		efsw::WatchID watchID = fileWatcher.addWatch( "/tmp", ul, true );
		
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
	} 
	catch( std::exception& e ) 
	{
		fprintf(stderr, "An exception has occurred: %s\n", e.what());
	}

	return 0;
}
