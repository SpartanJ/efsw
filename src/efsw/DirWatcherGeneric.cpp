#include <efsw/DirWatcherGeneric.hpp>
#include <efsw/FileSystem.hpp>

namespace efsw {

DirWatcherGeneric::DirWatcherGeneric( WatcherGeneric * ws, const std::string& directory, bool recursive ) :
	Watch( ws ),
	Directory( directory ),
	Recursive( recursive ),
	Deleted( false )
{
	/// Is this a recursive watch?
	if ( Watch->Directory != directory )
	{
		/// Get the real directory
		Directory = Watch->CurDirWatch->Directory + directory;
	}

	if ( NULL == Watch->DirWatch )
	{
		Watch->DirWatch = this;
	}

	Watch->CurDirWatch = this;

	FileSystem::dirAddSlashAtEnd( Directory );

	/// Why i'm doing this? stat in mingw32 doesn't work for directories if the dir path ends with a path slash
	std::string dir( Directory );
	FileSystem::dirRemoveSlashAtEnd( dir );
	DirInfo = FileInfo( dir );

	GetFiles( Directory, Files );

	FileInfoMap::iterator it = Files.begin();
	std::list<std::string> eraseFiles;

	/// Remove all non regular files and non directories
	for ( ; it != Files.end(); it++ )
	{
		if ( !it->second.isRegularFile() && !it->second.isDirectory() )
		{
			eraseFiles.push_back( it->first );
		}
	}

	for ( std::list<std::string>::iterator eit = eraseFiles.begin(); eit != eraseFiles.end(); eit++ )
	{
		Files.erase( *eit );
	}

	if ( Recursive )
	{
		/// Create the subdirectories watchers
		FileInfoMap::iterator it = Files.begin();

		for ( ; it != Files.end(); it++ )
		{
			if ( it->second.isDirectory() )
			{
				/// Check if the directory is a symbolic link
				std::string curPath;
				std::string link( FileSystem::getLinkRealPath( it->second.Filepath, curPath ) );

				if ( "" != link )
				{
					/// If it's a symlink check if the realpath exists as a watcher, or
					/// if the path is outside the current dir
					if ( Watch->isPath( link ) || Watch->WatcherImpl->linkAllowed( curPath, link ) )
					{
						continue;
					}
				}

				Directories[ it->first ] = new DirWatcherGeneric( ws, it->first, recursive );

				Watch->CurDirWatch = this;
			}
		}
	}
}

DirWatcherGeneric::~DirWatcherGeneric()
{
	/// If the directory was deleted mark the files as deleted
	if ( Deleted )
	{
		DirWatcherGeneric * oldWatch = Watch->CurDirWatch;

		Watch->CurDirWatch = this;

		FileInfoMap::iterator it;
		FileInfo fi;

		for ( it = Files.begin(); it != Files.end(); it++ )
		{
			fi = it->second;

			Watch->WatcherImpl->handleAction( Watch, it->first, Actions::Delete );
		}

		Watch->CurDirWatch = oldWatch;
	}

	DirWatchMap::iterator it = Directories.begin();

	for ( ; it != Directories.end(); it++ )
	{
		if ( Deleted )
		{
			/// If the directory was deleted, mark the flag for file deletion
			it->second->Deleted = true;
		}

		efSAFE_DELETE( it->second );
	}
}

void DirWatcherGeneric::watch()
{
	DirWatcherGeneric * oldWatch = Watch->CurDirWatch;

	FileInfo curFI( DirInfo.Filepath );
	bool dirchanged	= DirInfo != curFI;

	if ( dirchanged )
	{
		DirInfo			= curFI;
	}

	FileInfoMap files;

	GetFiles( Directory, files );

	if ( files.empty() && Files.empty() )
	{
		return;
	}

	FileInfoMap FilesCpy;

	/// If the directory not changed means that no file changed
	/// But a subdirectory could have changed

	if ( dirchanged )
	{
		FilesCpy = Files;
	}

	FileInfoMap::iterator it;
	DirWatchMap::iterator dit;
	DirWatcherGeneric * dw;
	FileInfo fi;

	if ( dirchanged )
	{
		FileInfoMap::iterator fif;

		for ( it = files.begin(); it != files.end(); it++ )
		{
			fi	= it->second;

			/// File existed before?
			fif = Files.find( it->first );

			if ( fif != Files.end() )
			{
				/// Erase from the file list copy
				FilesCpy.erase( it->first );

				/// File changed?
				if ( (*fif).second != fi )
				{
					/// Update the new file info
					Files[ it->first ] = fi;

					/// handle modified event
					Watch->CurDirWatch = this;
					Watch->WatcherImpl->handleAction( Watch, it->first, Actions::Modified );
				}
			}
			else if ( fi.isRegularFile() || fi.isDirectory() ) /// Only add regular files or directories
			{
				/// New file found
				Files[ it->first ] = fi;

				/// handle add event
				Watch->CurDirWatch = this;
				Watch->WatcherImpl->handleAction( Watch, it->first, Actions::Add );
			}

			/// Is directory and recursive?
			if ( Recursive && fi.isDirectory() )
			{
				/// Directory existed?
				dit = Directories.find( it->first );

				if ( dit != Directories.end() )
				{
					dw = dit->second;

					/// Just watch
					dw->watch();
				}
				else
				{
					/// Creates the new directory watcher of the subfolder and check for new files
					dw = new DirWatcherGeneric( Watch, it->first, Recursive );
					dw->watch();

					/// Add it to the list of directories
					Directories[ it->first ] = dw;
				}
			}
		}
	}
	else
	{
		/// If the dir didn't change, process the subdirectories looking for changes
		for ( dit = Directories.begin(); dit != Directories.end(); dit++ )
		{
			dw = dit->second;

			/// Just watch
			dw->watch();
		}
	}

	Watch->CurDirWatch = oldWatch;

	if ( !dirchanged )
	{
		return;
	}

	/// The files or directories that remains were deleted
	for ( it = FilesCpy.begin(); it != FilesCpy.end(); it++ )
	{
		fi = it->second;

		if ( fi.isDirectory() )
		{
			/// Folder deleted

			/// Search the folder, it should exists
			dit = Directories.find( it->first );

			if ( dit != Directories.end() )
			{
				dw = (*dit).second;;

				/// Flag it as deleted so it fire the event for every file inside deleted
				dw->Deleted = true;

				/// Delete the DirWatcherGeneric
				efSAFE_DELETE( dw );

				/// Remove the directory from the map
				Directories.erase( (*dit).first );
			}
		}

		/// File or Directory deleted
		Watch->CurDirWatch = this;

		/// handle delete event
		Watch->WatcherImpl->handleAction( Watch, it->first, Actions::Delete );

		/// Remove the file or directory from the list of files
		Files.erase( it->first );
	}

	Watch->CurDirWatch = oldWatch;
}

void DirWatcherGeneric::GetFiles( const std::string& directory, FileInfoMap& files )
{
	files = FileSystem::filesInfoFromPath( directory );
}

bool DirWatcherGeneric::isPath( std::string path )
{
	if ( Directory == path )
	{
		return true;
	}

	for ( DirWatchMap::iterator it = Directories.begin(); it != Directories.end(); it++ )
	{
		 if ( it->second->isPath( path ) )
		 {
			 return true;
		 }
	}

	return false;
}

} 
