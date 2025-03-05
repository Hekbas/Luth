project "glm"
	kind "StaticLib"
	language "C"
	cppdialect "C++20"
	architecture "x86_64"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
	
	includedirs { "source/glm" } 

	files
	{
		"source/glm/glm/**"
	}
	
	filter "system:windows"
		systemversion "latest"

		defines 
		{ 
			"_GLM_WIN32",
			"_CRT_SECURE_NO_WARNINGS"
		}

	filter "system:linux"
		systemversion "latest"
		pic "On"

		defines
		{
			"_GLM_X11"
		}	

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
