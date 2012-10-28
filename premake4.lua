solution "efsw"
	location("./make/" .. os.get() .. "/")
	targetdir("./bin")
	configurations { "debug", "release" }

	if os.is("windows") then
		osfiles = "src/efsw/platform/win/*.cpp"
	else
		osfiles = "src/efsw/platform/posix/*.cpp"
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

		configuration "debug"
			defines { "DEBUG" }
			flags { "Symbols" }
			targetname "efsw-test-debug"

		configuration "release"
			defines { "NDEBUG" }
			flags { "Optimize" }
			targetname "efsw-test-release"

	project "efsw-shared-lib"
		kind "SharedLib"
		language "C++"
		targetdir("./lib")
		includedirs { "include", "src" }
		files { "src/efsw/*.cpp", osfiles }
		
		configuration "debug"
			defines { "DEBUG" }
			flags { "Symbols" }
			targetname "efsw-debug"

		configuration "release"
			defines { "NDEBUG" }
			flags { "Optimize" }
			targetname "efsw"
