project "Luthien"
   kind "StaticLib"
   language "C++"
   cppdialect "C++20"

   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   pchheader "lepch.h"
   pchsource "source/lepch.cpp"

   buildoptions { "/utf-8" }

   defines
   {
      "GLFW_INCLUDE_NONE",
      "FMT_HEADER_ONLY=1"
   }

   files
   {
      "source/**.h",
      "source/**.cpp",
   }

   includedirs
   {
      "source",
      "%{wks.location}/luth/source",
      "%{wks.location}/luth/extern/source",
      "%{wks.location}/luth/extern/config-headers",
      IncludeDir["assimp"],
      IncludeDir["glfw"],
      IncludeDir["glm"],
      IncludeDir["imgui"],
      IncludeDir["imguizmo"],
      IncludeDir["spdlog"],
      IncludeDir["tracy"],
      IncludeDir["vulkan"],
      IncludeDir["spirv_cross"]
   }

   libdirs
   {
      "%{wks.location}/luth/extern/source/vulkan/lib",
   }

   local vulkanSDK = os.getenv("VULKAN_SDK")
   if vulkanSDK then
      libdirs { vulkanSDK .. "/Lib" }
   end

   links
   {
      "Luth",
      "imgui",
      "ImGuizmo"
   }

   filter "configurations:Debug"
      defines { "DEBUG", "TRACY_ENABLE", "TRACY_FIBERS" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "RELEASE", "TRACY_ENABLE", "TRACY_FIBERS" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      defines { "DIST" }
      runtime "Release"
      optimize "on"
