#include <efsw/efsw.hpp>
#include <efsw/Debug.hpp>

namespace efsw { namespace Errors {

static std::string LastError = "";
static Error LastErrorCode = NoError;

std::string Log::getLastErrorLog() {
	return LastError;
}

Error Log::getLastErrorCode() {
	return LastErrorCode;
}

void Log::clearLastError() {
	LastErrorCode = NoError;
	LastError = "";
}

Error Log::createLastError( Error err, std::string log ) {
	switch ( err ) {
		case FileNotFound:
			LastError = "File not found ( " + log + " )";
			break;
		case FileRepeated:
			LastError = "File repeated in watches ( " + log + " )";
			break;
		case FileOutOfScope:
			LastError = "Symlink file out of scope ( " + log + " )";
			break;
		case FileRemote:
			LastError =
				"File is located in a remote file system, use a generic watcher. ( " + log + " )";
			break;
		case WinReadDirectoryChangesFailed:
			LastError = "ReadDirectoryChanges failed ( " + log + " )";
			break;
		case Unspecified:
		default:
			LastError = log;
	}

	efDEBUG( "%s\n", LastError );
	return err;
}

}} // namespace efsw::Errors
