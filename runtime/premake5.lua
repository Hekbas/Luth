project "Runtime"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++20"
   targetname "Luthien"

   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

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

   filter "system:windows"
      files
      {
         "resource.h",
         "Luthien.rc",
         "icons/Luth.ico"
      }
   filter {}

   includedirs
   {
      "source",
      "%{wks.location}/luth/source",
      "%{wks.location}/luthien/source",
      "%{wks.location}/luth/extern/source",
      "%{wks.location}/luth/extern/config-headers",
      IncludeDir["assimp"],
      IncludeDir["glfw"],
      IncludeDir["glm"],
      IncludeDir["imgui"],
      IncludeDir["spdlog"],
      IncludeDir["vulkan"]
   }

   libdirs
   {
      LibraryDir["vulkan"]
   }

   postbuildcommands
   {
      "{COPY} " .. LibraryDir["vulkan"] .. "/shaderc_shared.dll %{cfg.targetdir}"
   }

   links
   {
      "Luth",
      "Luthien",
      "vulkan-1",
      "shaderc_shared"
   }

   filter "configurations:Debug"
      defines { "LUTH_BUILD_DEBUG", "TRACY_ENABLE", "TRACY_FIBERS", "TRACY_ON_DEMAND" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "LUTH_BUILD_RELEASE", "TRACY_ENABLE", "TRACY_FIBERS", "TRACY_ON_DEMAND" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      defines { "LUTH_BUILD_DIST" }
      runtime "Release"
      optimize "on"
      