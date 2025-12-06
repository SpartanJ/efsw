newoption { trigger = "verbose", description = "Build efsw with verbose mode." }
newoption { trigger = "strip-symbols", description = "Strip debugging symbols in other file ( only for relwithdbginfo configuration )." }
newoption { trigger = "thread-sanitizer", description ="Compile with ThreadSanitizer." }
newoption { trigger = "address-sanitizer", description ="Compile with AddressSanitizer." }

efsw_major_version	= "1"
efsw_minor_version	= "5"
efsw_patch_version	= "0"
efsw_version		= efsw_major_version .. "." .. efsw_minor_version .. "." .. efsw_patch_version

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

		if not os.istarget("windows") then
			buildoptions{ "-fPIC" }
		end
	else
		defines { "_SCL_SECURE_NO_WARNINGS" }
	end

	if _OPTIONS["thread-sanitizer"] then
		buildoptions { "-fsanitize=thread" }
		linkoptions { "-fsanitize=thread" }
		if not os.istarget("macosx") then
			links { "tsan" }
		end
	end

	if _OPTIONS["address-sanitizer"] then
		buildoptions { "-fsanitize=address" }
		linkoptions { "-fsanitize=address" }
		if not os.istarget("macosx") then
			links { "asan" }
		end
	end
end

function conf_links()
	if not os.istarget("windows") and not os.istarget("haiku") then
		links { "pthread" }
	end

	if os.istarget("macosx") then
		links { "CoreFoundation.framework", "CoreServices.framework" }
	end
end

function conf_excludes()
	if os.istarget("windows") then
		excludes { "src/efsw/WatcherKqueue.cpp", "src/efsw/WatcherFSEvents.cpp", "src/efsw/WatcherInotify.cpp", "src/efsw/FileWatcherKqueue.cpp", "src/efsw/FileWatcherInotify.cpp", "src/efsw/FileWatcherFSEvents.cpp" }
	elseif os.istarget("linux") then
		excludes { "src/efsw/WatcherKqueue.cpp", "src/efsw/WatcherFSEvents.cpp", "src/efsw/WatcherWin32.cpp", "src/efsw/FileWatcherKqueue.cpp", "src/efsw/FileWatcherWin32.cpp", "src/efsw/FileWatcherFSEvents.cpp" }
	elseif os.istarget("macosx") then
		excludes { "src/efsw/WatcherInotify.cpp", "src/efsw/WatcherWin32.cpp", "src/efsw/FileWatcherInotify.cpp", "src/efsw/FileWatcherWin32.cpp" }
	elseif os.istarget("bsd") then
		excludes { "src/efsw/WatcherInotify.cpp", "src/efsw/WatcherWin32.cpp", "src/efsw/WatcherFSEvents.cpp", "src/efsw/FileWatcherInotify.cpp", "src/efsw/FileWatcherWin32.cpp", "src/efsw/FileWatcherFSEvents.cpp" }
	end
end

workspace "efsw"
	location("./make/" .. os.target() .. "/")
	targetdir("./bin")
	configurations { "debug", "release", "relwithdbginfo" }
	platforms { "x86_64", "x86", "ARM", "ARM64" }

	if os.istarget("windows") then
		osfiles = "src/efsw/platform/win/*.cpp"
	else
		osfiles = "src/efsw/platform/posix/*.cpp"
	end

	-- Activates verbose mode
	if _OPTIONS["verbose"] then
		defines { "EFSW_VERBOSE" }
	end

	cppdialect "C++11"

	objdir("obj/" .. os.target() .. "/")

	filter "platforms:x86"
		architecture "x86"

	filter "platforms:x86_64"
		architecture "x86_64"

	filter "platforms:arm"
		architecture "ARM"

	filter "platforms:arm64"
		architecture "ARM64"

	project "efsw-static-lib"
		kind "StaticLib"
		language "C++"
		targetdir("./lib")
		includedirs { "include", "src" }
		files { "src/efsw/*.cpp", osfiles }
		conf_excludes()

		filter "configurations:debug"
			defines { "DEBUG" }
			symbols "On"
			targetname "efsw-static-debug"
			conf_warnings()

		filter "configurations:release"
			defines { "NDEBUG" }
			optimize "On"
			targetname "efsw-static-release"
			conf_warnings()

		filter "configurations:relwithdbginfo"
			defines { "NDEBUG" }
			symbols "On"
			optimize "On"
			targetname "efsw-static-reldbginfo"
			conf_warnings()

	project "efsw-test"
		kind "ConsoleApp"
		language "C++"
		links { "efsw-static-lib" }
		files { "src/test/*.cpp" }
		includedirs { "include", "src" }
		conf_links()

		filter "configurations:debug"
			defines { "DEBUG" }
			symbols "On"
			targetname "efsw-test-debug"
			conf_warnings()

		filter "configurations:release"
			defines { "NDEBUG" }
			optimize "On"
			targetname "efsw-test-release"
			conf_warnings()

		filter "configurations:relwithdbginfo"
			defines { "NDEBUG" }
			symbols "On"
			optimize "On"
			targetname "efsw-test-reldbginfo"
			conf_warnings()

	project "efsw-test-stdc"
		kind "ConsoleApp"
		language "C"
		links { "efsw-shared-lib" }
		files { "src/test/*.c" }
		includedirs { "include", "src" }
		conf_links()

		filter "configurations:debug"
			defines { "DEBUG" }
			symbols "On"
			targetname "efsw-test-stdc-debug"
			conf_warnings()

		filter "configurations:release"
			defines { "NDEBUG" }
			optimize "On"
			targetname "efsw-test-stdc-release"
			conf_warnings()

		filter "configurations:relwithdbginfo"
			defines { "NDEBUG" }
			symbols "On"
			optimize "On"
			targetname "efsw-test-stdc-reldbginfo"
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

		filter "configurations:debug"
			defines { "DEBUG" }
			symbols "On"
			targetname "efsw-debug"
			conf_warnings()

		filter "configurations:release"
			defines { "NDEBUG" }
			optimize "On"
			targetname "efsw"
			conf_warnings()

		filter "configurations:relwithdbginfo"
			defines { "NDEBUG" }
			symbols "On"
			optimize "On"
			targetname "efsw"
			conf_warnings()

			if os.istarget("linux") or os.istarget("bsd") or os.istarget("haiku") then
				targetextension ( ".so." .. efsw_version )
				postbuildcommands { "sh ../../project/build.reldbginfo.sh " .. efsw_major_version .. " " .. efsw_minor_version .. " " .. efsw_patch_version .. " " .. iif( _OPTIONS["strip-symbols"], "strip-symbols", "" ) }
			end
