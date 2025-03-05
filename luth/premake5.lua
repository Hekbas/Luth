project "Luth"
   kind "StaticLib"
   language "C++"
   cppdialect "C++20"

   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   pchheader "luthpch.h"
   pchsource "source/luthpch.cpp"

   buildoptions { "/utf-8" }

   defines
   {
      "GLFW_INCLUDE_NONE",
      "FMT_HEADER_ONLY=1"
   }

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
      IncludeDir["spdlog"],
      IncludeDir["vulkan"]
   }

   libdirs
   {
      "extern/source/vulkan/lib"
   }

   links
   {
      "assimp",
      "glad",
      "glfw",
      "glm",
      "imgui",
      "vulkan-1"
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
      