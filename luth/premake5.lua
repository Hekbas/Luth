project "Luth"
   kind "StaticLib"
   language "C++"
   cppdialect "C++20"

   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   pchheader "luthpch.h"
   pchsource "source/luthpch.cpp"

   buildoptions { "/utf-8" }

   files
   {
      "source/**.h",
      "source/**.cpp"
   }
   
   includedirs
   {
      "source",
      "extern/source",
      IncludeDir["assimp"],
      IncludeDir["glad"],
      IncludeDir["glfw"],
      IncludeDir["glm"],
      IncludeDir["imgui"],
      IncludeDir["spdlog"]
   }

   links
   {
      "assimp",
      "glad",
      "glfw",
      "glm",
      "imgui"
   }

   filter "configurations:Debug"
      defines { "DEBUG" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "RELEASE" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      defines { "DIST" }
      runtime "Release"
      optimize "on"
      