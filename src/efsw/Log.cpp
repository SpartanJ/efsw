#include <efsw/efsw.hpp>

namespace efsw { namespace Errors {

static std::string LastError;

std::string Log::getLastErrorLog()
{
	return LastError;
}

Error Log::createLastError( Error err, std::string log )
{
	switch ( err )
	{
		case FileNotFound:	LastError = "File not found (" + log + ")";
		case FileRepeated:	LastError = "File reapeated in watches ( " + log + ")";
		case Unspecified:
		default:			LastError = log;
	}

	return err;
}

}}
