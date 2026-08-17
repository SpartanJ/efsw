#include <efsw/Debug.hpp>
#include <efsw/FileSystem.hpp>
#include <efsw/String.hpp>
#include <efsw/WatcherWin32.hpp>

#if EFSW_PLATFORM == EFSW_PLATFORM_WIN32

#include <algorithm>
#include <utility>

namespace efsw {

static constexpr ULONGLONG pendingMoveTimeoutMs = 100;

static std::string parentPath( const std::string& path ) {
	std::size_t separator = path.find_last_of( "/\\" );
	return separator == std::string::npos ? "" : path.substr( 0, separator + 1 );
}

struct EFSW_FILE_NOTIFY_EXTENDED_INFORMATION_EX {
	DWORD NextEntryOffset;
	DWORD Action;
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastModificationTime;
	LARGE_INTEGER LastChangeTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER AllocatedLength;
	LARGE_INTEGER FileSize;
	DWORD FileAttributes;
	DWORD ReparsePointTag;
	LARGE_INTEGER FileId;
	LARGE_INTEGER ParentFileId;
	DWORD FileNameLength;
	WCHAR FileName[1];
};

typedef EFSW_FILE_NOTIFY_EXTENDED_INFORMATION_EX* EFSW_PFILE_NOTIFY_EXTENDED_INFORMATION_EX;

typedef BOOL( WINAPI* EFSW_LPREADDIRECTORYCHANGESEXW )(
	HANDLE hDirectory, LPVOID lpBuffer, DWORD nBufferLength, BOOL bWatchSubtree,
	DWORD dwNotifyFilter, LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped,
	LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine,
	DWORD ReadDirectoryNotifyInformationClass );

static EFSW_LPREADDIRECTORYCHANGESEXW pReadDirectoryChangesExW = NULL;

#define EFSW_ReadDirectoryNotifyExtendedInformation 2

static void initReadDirectoryChangesEx() {
	static bool hasInit = false;
	if ( !hasInit ) {
		hasInit = true;

		HMODULE hModule = GetModuleHandleW( L"Kernel32.dll" );
		if ( !hModule )
			return;

		pReadDirectoryChangesExW =
			(EFSW_LPREADDIRECTORYCHANGESEXW)GetProcAddress( hModule, "ReadDirectoryChangesExW" );
	}
}

void WatchCallbackOld( WatcherWin32* pWatch ) {
	PFILE_NOTIFY_INFORMATION pNotify;
	size_t offset = 0;
	do {
		bool skip = false;

		pNotify = (PFILE_NOTIFY_INFORMATION)&pWatch->Buffer[offset];
		offset += pNotify->NextEntryOffset;
		int count =
			WideCharToMultiByte( CP_UTF8, 0, pNotify->FileName,
								 pNotify->FileNameLength / sizeof( WCHAR ), NULL, 0, NULL, NULL );
		if ( count == 0 )
			continue;

		std::string nfile( count, '\0' );

		count = WideCharToMultiByte( CP_UTF8, 0, pNotify->FileName,
									 pNotify->FileNameLength / sizeof( WCHAR ), &nfile[0], count,
									 NULL, NULL );

		if ( FILE_ACTION_MODIFIED == pNotify->Action ) {
			FileInfo fifile( std::string( pWatch->DirName ) + nfile );

			if ( pWatch->LastModifiedEvent.file.ModificationTime == fifile.ModificationTime &&
				 pWatch->LastModifiedEvent.file.Size == fifile.Size &&
				 pWatch->LastModifiedEvent.fileName == nfile ) {
				skip = true;
			}

			pWatch->LastModifiedEvent.fileName = nfile;
			pWatch->LastModifiedEvent.file = fifile;
		}

		if ( !skip ) {
			pWatch->Watch->handleAction( pWatch, nfile, pNotify->Action );
		}
	} while ( pNotify->NextEntryOffset != 0 );
}

void WatchCallbackEx( WatcherWin32* pWatch ) {
	pWatch->ExtendedEvents.clear();
	EFSW_PFILE_NOTIFY_EXTENDED_INFORMATION_EX pNotify;
	size_t offset = 0;
	do {
		pNotify = (EFSW_PFILE_NOTIFY_EXTENDED_INFORMATION_EX)&pWatch->Buffer[offset];
		offset += pNotify->NextEntryOffset;
		int count =
			WideCharToMultiByte( CP_UTF8, 0, pNotify->FileName,
								 pNotify->FileNameLength / sizeof( WCHAR ), NULL, 0, NULL, NULL );
		if ( count == 0 )
			continue;

		std::string nfile( count, '\0' );

		count = WideCharToMultiByte( CP_UTF8, 0, pNotify->FileName,
									 pNotify->FileNameLength / sizeof( WCHAR ), &nfile[0], count,
									 NULL, NULL );
		if ( count != 0 ) {
			ExtendedEventWin32 event;
			event.Action = pNotify->Action;
			event.FileName = std::move( nfile );
			event.FileId = pNotify->FileId;
			pWatch->ExtendedEvents.emplace_back( std::move( event ) );
		}
	} while ( pNotify->NextEntryOffset != 0 );

	for ( size_t i = 0; i < pWatch->ExtendedEvents.size(); i++ ) {
		const ExtendedEventWin32& event = pWatch->ExtendedEvents[i];
		const std::string& nfile = event.FileName;
		bool skip = false;

		if ( pWatch->ReportCrossDirectoryMoves && pWatch->Recursive &&
			 event.Action == FILE_ACTION_REMOVED && event.FileId.QuadPart != 0 ) {
			PendingRenameWin32 pending;
			pending.FileName = nfile;
			pending.FileId = event.FileId;
			pending.CreatedAt = GetTickCount64();
			pWatch->PendingRemovals.emplace_back( std::move( pending ) );
			continue;
		}

		if ( pWatch->ReportCrossDirectoryMoves && pWatch->Recursive &&
			 event.Action == FILE_ACTION_ADDED && event.FileId.QuadPart != 0 ) {
			size_t matches = 0;
			auto source = pWatch->PendingRemovals.end();
			for ( auto it = pWatch->PendingRemovals.begin(); it != pWatch->PendingRemovals.end();
				  ++it ) {
				if ( it->FileId.QuadPart == event.FileId.QuadPart ) {
					source = it;
					matches++;
				}
			}

			if ( matches == 1 ) {
				FileInfo destination( std::string( pWatch->DirName ) + nfile );
				bool identityMatches =
					destination.Inode != 0 &&
					destination.Inode == static_cast<Uint64>( event.FileId.QuadPart );
				bool unambiguousIdentity =
					destination.isDirectory() ||
					( destination.isRegularFile() && destination.LinkCount == 1 );
				if ( parentPath( source->FileName ) != parentPath( nfile ) && identityMatches &&
					 unambiguousIdentity ) {
					std::string oldFile( source->FileName );
					pWatch->PendingRemovals.erase( source );
					pWatch->Watch->handleAction( pWatch, nfile, FILE_ACTION_RENAMED_NEW_NAME,
												 oldFile );
					continue;
				}
			}

			if ( matches != 0 ) {
				for ( auto it = pWatch->PendingRemovals.begin();
					  it != pWatch->PendingRemovals.end(); ) {
					if ( it->FileId.QuadPart == event.FileId.QuadPart ) {
						pWatch->Watch->handleAction( pWatch, it->FileName, FILE_ACTION_REMOVED );
						it = pWatch->PendingRemovals.erase( it );
					} else {
						++it;
					}
				}
			}
		}

		if ( FILE_ACTION_MODIFIED == event.Action ) {
			FileInfo fifile( std::string( pWatch->DirName ) + nfile );

			if ( pWatch->LastModifiedEvent.file.ModificationTime == fifile.ModificationTime &&
				 pWatch->LastModifiedEvent.file.Size == fifile.Size &&
				 pWatch->LastModifiedEvent.fileName == nfile ) {
				skip = true;
			}

			pWatch->LastModifiedEvent.fileName = nfile;
			pWatch->LastModifiedEvent.file = fifile;
		} else if ( FILE_ACTION_RENAMED_OLD_NAME == event.Action ) {
			PendingRenameWin32 pending;
			pending.FileName = nfile;
			pending.FileId = event.FileId;
			pending.CreatedAt = GetTickCount64();
			pWatch->PendingRenames.emplace_back( std::move( pending ) );
			skip = true;
		} else if ( FILE_ACTION_RENAMED_NEW_NAME == event.Action ) {
			std::string oldFile;

			for ( auto it = pWatch->PendingRenames.begin(); it != pWatch->PendingRenames.end();
				  ++it ) {
				if ( it->FileId.QuadPart == event.FileId.QuadPart ) {
					oldFile = it->FileName;
					pWatch->PendingRenames.erase( it );
					break;
				}
			}

			if ( oldFile.empty() ) {
				pWatch->Watch->handleAction( pWatch, nfile, FILE_ACTION_ADDED );
				skip = true;
			} else {
				pWatch->Watch->handleAction( pWatch, nfile, FILE_ACTION_RENAMED_NEW_NAME, oldFile );
				skip = true;
			}
		}

		if ( !skip ) {
			pWatch->Watch->handleAction( pWatch, nfile, event.Action );
		}
	}
}

DWORD PendingMoveWaitTimeout( const WatcherWin32* pWatch ) {
	const ULONGLONG now = GetTickCount64();
	DWORD timeout = INFINITE;

	auto updateTimeout = [&]( const std::vector<PendingRenameWin32>& pending ) {
		for ( const PendingRenameWin32& event : pending ) {
			const ULONGLONG elapsed = now - event.CreatedAt;
			const DWORD remaining =
				elapsed >= pendingMoveTimeoutMs
					? 0
					: static_cast<DWORD>( pendingMoveTimeoutMs - elapsed );
			if ( remaining < timeout )
				timeout = remaining;
		}
	};

	updateTimeout( pWatch->PendingRenames );
	updateTimeout( pWatch->PendingRemovals );
	return timeout;
}

void FlushPendingMoves( WatcherWin32* pWatch ) {
	const ULONGLONG now = GetTickCount64();

	auto flush = [&]( std::vector<PendingRenameWin32>& pending ) {
		for ( auto it = pending.begin(); it != pending.end(); ) {
			if ( now - it->CreatedAt >= pendingMoveTimeoutMs ) {
				pWatch->Watch->handleAction( pWatch, it->FileName, FILE_ACTION_REMOVED );
				it = pending.erase( it );
			} else {
				++it;
			}
		}
	};

	flush( pWatch->PendingRenames );
	flush( pWatch->PendingRemovals );
}

/// Unpacks events and passes them to a user defined callback.
void CALLBACK WatchCallback( DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped ) {
	if ( NULL == lpOverlapped ) {
		return;
	}

	WatcherStructWin32* tWatch = (WatcherStructWin32*)lpOverlapped;
	WatcherWin32* pWatch = tWatch->Watch;

	if ( dwNumberOfBytesTransfered == 0 ) {
		if ( nullptr != pWatch && !pWatch->StopNow ) {
			/// Missed file actions due to buffer overflowed
			pWatch->PendingRenames.clear();
			pWatch->PendingRemovals.clear();
			std::string dir = pWatch->DirName;
			FileSystem::dirRemoveSlashAtEnd( dir );
			pWatch->Listener->handleMissedFileActions( pWatch->ID, dir );
			RefreshWatch( tWatch );
		}
		return;
	}

	// Fork watch depending on the Windows API supported
	if ( pWatch->Extended ) {
		WatchCallbackEx( pWatch );
	} else {
		WatchCallbackOld( pWatch );
	}

	if ( !pWatch->StopNow ) {
		RefreshWatch( tWatch );
	}
}

/// Refreshes the directory monitoring.
RefreshResult RefreshWatch( WatcherStructWin32* pWatch ) {
	initReadDirectoryChangesEx();

	bool bRet = false;
	RefreshResult ret = RefreshResult::Failed;
	pWatch->Watch->Extended = false;

	if ( pReadDirectoryChangesExW ) {
		bRet =
			pReadDirectoryChangesExW( pWatch->Watch->DirHandle, pWatch->Watch->Buffer.data(),
									  (DWORD)pWatch->Watch->Buffer.size(), pWatch->Watch->Recursive,
									  pWatch->Watch->NotifyFilter, NULL, &pWatch->Overlapped, NULL,
									  EFSW_ReadDirectoryNotifyExtendedInformation ) != 0;
		if ( bRet ) {
			ret = RefreshResult::SucessEx;
			pWatch->Watch->Extended = true;
		}
	}

	if ( !bRet ) {
		bRet = ReadDirectoryChangesW( pWatch->Watch->DirHandle, pWatch->Watch->Buffer.data(),
									  (DWORD)pWatch->Watch->Buffer.size(), pWatch->Watch->Recursive,
									  pWatch->Watch->NotifyFilter, NULL, &pWatch->Overlapped,
									  NULL ) != 0;

		if ( bRet )
			ret = RefreshResult::Success;
	}

	if ( !bRet ) {
		std::string error = std::to_string( GetLastError() );
		Errors::Log::createLastError( Errors::WatcherFailed, error );
	}

	return ret;
}

/// Stops monitoring a directory while keeping its OVERLAPPED storage alive until completion.
void StopWatch( WatcherStructWin32* pWatch ) {
	if ( pWatch ) {
		WatcherWin32* tWatch = pWatch->Watch;
		tWatch->StopNow = true;
		if ( tWatch->DirHandle != INVALID_HANDLE_VALUE ) {
			CancelIoEx( tWatch->DirHandle, &pWatch->Overlapped );
			CloseHandle( tWatch->DirHandle );
			tWatch->DirHandle = INVALID_HANDLE_VALUE;
		}
	}
}

/// Releases a stopped watch after its asynchronous operation has completed.
void DestroyWatch( WatcherStructWin32* pWatch ) {
	if ( pWatch ) {
		efSAFE_DELETE_ARRAY( pWatch->Watch->DirName );
		efSAFE_DELETE( pWatch->Watch );
		efSAFE_DELETE( pWatch );
	}
}

/// Starts monitoring a directory.
WatcherStructWin32* CreateWatch( LPCWSTR szDirectory, bool recursive, DWORD bufferSize,
								 DWORD notifyFilter, HANDLE iocp, bool preventDeletion ) {
	WatcherStructWin32* tWatch = new WatcherStructWin32();
	WatcherWin32* pWatch = new WatcherWin32( bufferSize );
	if ( tWatch )
		tWatch->Watch = pWatch;

	DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
	if ( !preventDeletion ) {
		shareMode |= FILE_SHARE_DELETE;
	}

	pWatch->DirHandle = CreateFileW( szDirectory, GENERIC_READ, shareMode, NULL, OPEN_EXISTING,
									 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL );

	if ( pWatch->DirHandle != INVALID_HANDLE_VALUE &&
		 CreateIoCompletionPort( pWatch->DirHandle, iocp, 0, 1 ) ) {
		pWatch->NotifyFilter = notifyFilter;
		pWatch->Recursive = recursive;

		if ( RefreshResult::Failed != RefreshWatch( tWatch ) ) {
			return tWatch;
		}
	}

	CloseHandle( pWatch->DirHandle );
	efSAFE_DELETE( pWatch->Watch );
	efSAFE_DELETE( tWatch );
	return NULL;
}

} // namespace efsw

#endif
