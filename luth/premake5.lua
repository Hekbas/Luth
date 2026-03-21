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
      "FMT_HEADER_ONLY=1",
      "LUTH_SPIRV_CROSS_ENABLED=1",
      "SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS"
   }

   files
   {
      "source/**.h",
      "source/**.cpp",
   }
   
   includedirs
   {
      "source",
      "extern/source",
      "extern/config-headers",
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
      "extern/source/vulkan/lib",
   }

   local vulkanSDK = os.getenv("VULKAN_SDK")
   if vulkanSDK then
      libdirs { vulkanSDK .. "/Lib" }
   end

   links
   {
      "assimp",
      "glfw",
      "glm",
      "imgui",
      "ImGuizmo",
      "Tracy",
      "vulkan-1",
      "shaderc_shared",
      "spirv-cross",
      "ws2_32",
      "dbghelp"
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
      