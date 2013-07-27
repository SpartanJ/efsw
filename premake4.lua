function args_contains( element )
  for _, value in pairs(_ARGS) do
    if value == element then
      return true
    end
  end
  return false
end

function string.starts(String,Start)
	if ( _ACTION ) then
		return string.sub(String,1,string.len(Start))==Start
	end
	
	return false
end

function is_vs()
	return ( string.starts(_ACTION,"vs") )
end

function conf_warnings()
	if not is_vs() then
		buildoptions{ "-Wall -Wno-long-long" }
	else
		defines { "_SCL_SECURE_NO_WARNINGS" }
	end
end

function conf_links()
	if not os.is("windows") and not os.is("haiku") then
		links { "pthread" }
	end

	if os.is("macosx") then
		links { "CoreFoundation.framework", "CoreServices.framework" }
	end
end

function conf_excludes()
	if os.is("windows") then
		excludes { "src/efsw/WatcherKqueue.cpp", "src/efsw/WatcherFSEvents.cpp", "src/efsw/WatcherInotify.cpp", "src/efsw/FileWatcherKqueue.cpp", "src/efsw/FileWatcherInotify.cpp", "src/efsw/FileWatcherFSEvents.cpp" }
	elseif os.is("linux") then
		excludes { "src/efsw/WatcherKqueue.cpp", "src/efsw/WatcherFSEvents.cpp", "src/efsw/WatcherWin32.cpp", "src/efsw/FileWatcherKqueue.cpp", "src/efsw/FileWatcherWin32.cpp", "src/efsw/FileWatcherFSEvents.cpp" }
	elseif os.is("macosx") then
		excludes { "src/efsw/WatcherInotify.cpp", "src/efsw/WatcherWin32.cpp", "src/efsw/FileWatcherInotify.cpp", "src/efsw/FileWatcherWin32.cpp" }
	elseif os.is("freebsd") then
		excludes { "src/efsw/WatcherInotify.cpp", "src/efsw/WatcherWin32.cpp", "src/efsw/WatcherFSEvents.cpp", "src/efsw/FileWatcherInotify.cpp", "src/efsw/FileWatcherWin32.cpp", "src/efsw/FileWatcherFSEvents.cpp" }
	end
end

solution "efsw"
	location("./make/" .. os.get() .. "/")
	targetdir("./bin")
	configurations { "debug", "release" }

	if os.is("windows") then
		osfiles = "src/efsw/platform/win/*.cpp"
	else
		osfiles = "src/efsw/platform/posix/*.cpp"
	end
			
	-- This is for testing in other platforms that don't support kqueue
	if args_contains( "kqueue" ) then
		links { "kqueue" }
		defines { "EFSW_KQUEUE" }
		printf("Forced Kqueue backend build.")
	end
	
	-- Activates verbose mode
	if args_contains( "verbose" ) then
		defines { "EFSW_VERBOSE" }
	end

	if os.is("macosx") then
		-- Premake 4.4 needed for this
		if not string.match(_PREMAKE_VERSION, "^4.[123]") then
			local ver = os.getversion();

			if not ( ver.majorversion >= 10 and ver.minorversion >= 5 ) then
				defines { "EFSW_FSEVENTS_NOT_SUPPORTED" }
			end
		end
	end

	objdir("obj/" .. os.get() .. "/")
	
	project "efsw-static-lib"
		kind "StaticLib"
		language "C++"
		targetdir("./lib")
		includedirs { "include", "src" }
		files { "src/efsw/*.cpp", osfiles }
		conf_excludes()
		
		configuration "debug"
			defines { "DEBUG" }
			flags { "Symbols" }
			targetname "efsw-static-debug"
			conf_warnings()

		configuration "release"
			defines { "NDEBUG" }
			flags { "Optimize" }
			targetname "efsw-static-release"
			conf_warnings()
	
	project "efsw-test"
		kind "ConsoleApp"
		language "C++"
		links { "efsw-static-lib" }
		files { "src/test/*.cpp" }
		includedirs { "include", "src" }
		conf_links()
		
		configuration "debug"
			defines { "DEBUG" }
			flags { "Symbols" }
			targetname "efsw-test-debug"
			conf_warnings()

		configuration "release"
			defines { "NDEBUG" }
			flags { "Optimize" }
			targetname "efsw-test-release"
			conf_warnings()

	project "efsw-shared-lib"
		kind "SharedLib"
		language "C++"
		targetdir("./lib")
		includedirs { "include", "src" }
		files { "src/efsw/*.cpp", osfiles }
		defines { "EFSW_DYNAMIC", "EFSW_EXPORTS" }
		conf_excludes()
		conf_links()
		
		configuration "debug"
			defines { "DEBUG" }
			flags { "Symbols" }
			targetname "efsw-debug"
			conf_warnings()

		configuration "release"
			defines { "NDEBUG" }
			flags { "Optimize" }
			targetname "efsw"
			conf_warnings()
