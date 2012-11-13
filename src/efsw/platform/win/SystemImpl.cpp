#include <efsw/platform/win/SystemImpl.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdlib>

namespace efsw { namespace Platform {

void System::sleep( const unsigned long& ms )
{
	::Sleep( ms );
}

std::string System::getProcessPath() {
#ifdef UNICODE
	// Get path to executable:
	char szDrive[_MAX_DRIVE];
	char szDir[_MAX_DIR];
	char szFilename[_MAX_DIR];
	char szExt[_MAX_DIR];
	std::wstring dllName( _MAX_DIR, 0 );

	GetModuleFileName(0, &dllName[0], _MAX_PATH);

	std::string dllstrName( String( dllName ).ToUtf8() );

	#ifdef EE_COMPILER_MSVC
	_splitpath_s( dllstrName.c_str(), szDrive, _MAX_DRIVE, szDir, _MAX_DIR, szFilename, _MAX_DIR, szExt, _MAX_DIR );
	#else
	_splitpath(szDllName, szDrive, szDir, szFilename, szExt);
	#endif

	return std::string( szDrive ) + std::string( szDir );
#else
	// Get path to executable:
	TCHAR szDllName[_MAX_PATH];
	TCHAR szDrive[_MAX_DRIVE];
	TCHAR szDir[_MAX_DIR];
	TCHAR szFilename[_MAX_DIR];
	TCHAR szExt[_MAX_DIR];
	GetModuleFileName(0, szDllName, _MAX_PATH);

	#ifdef EFSW_COMPILER_MSVC
	_splitpath_s(szDllName, szDrive, _MAX_DRIVE, szDir, _MAX_DIR, szFilename, _MAX_DIR, szExt, _MAX_DIR );
	#else
	_splitpath(szDllName, szDrive, szDir, szFilename, szExt);
	#endif

	return std::string( szDrive ) + std::string( szDir );
#endif
}

void System::maxFD()
{
}

Uint64 System::getMaxFD()
{	// Number of ReadDirectory per thread
	return 60;
}

}}

#endif
