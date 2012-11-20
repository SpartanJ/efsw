function args_contains( element )
  for _, value in pairs(_ARGS) do
    if value == element then
      return true
    end
  end
  return false
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

		configuration "debug"
			defines { "DEBUG" }
			flags { "Symbols" }
			buildoptions{ "-Wall -pedantic -Wno-long-long" }
			targetname "efsw-static-debug"

		configuration "release"
			defines { "NDEBUG" }
			flags { "Optimize" }
			targetname "efsw-static-release"
	
	project "efsw-test"
		kind "ConsoleApp"
		language "C++"
		links { "efsw-static-lib" }
		files { "src/test/*.cpp" }
		includedirs { "include", "src" }
		
		if not os.is("windows") and not os.is("haiku") then
			links { "pthread" }
		end

		if os.is("macosx") then
			links { "CoreFoundation.framework", "CoreServices.framework" }
		end

		configuration "debug"
			defines { "DEBUG" }
			flags { "Symbols" }
			buildoptions{ "-Wall" }
			targetname "efsw-test-debug"

		configuration "release"
			defines { "NDEBUG" }
			flags { "Optimize" }
			buildoptions{ "-Wall" }
			targetname "efsw-test-release"

	project "efsw-shared-lib"
		kind "SharedLib"
		language "C++"
		targetdir("./lib")
		includedirs { "include", "src" }
		files { "src/efsw/*.cpp", osfiles }
		defines { "EFSW_DYNAMIC", "EFSW_EXPORTS" }
		
		configuration "debug"
			defines { "DEBUG" }
			buildoptions{ "-Wall" }
			flags { "Symbols" }
			targetname "efsw-debug"

		configuration "release"
			defines { "NDEBUG" }
			flags { "Optimize" }
			buildoptions{ "-Wall" }
			targetname "efsw"
