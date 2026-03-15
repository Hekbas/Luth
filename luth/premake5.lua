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
      IncludeDir["vulkan"]
   }

   libdirs
   {
      "extern/source/vulkan/lib",
      os.getenv("VULKAN_SDK") .. "/Lib"
   }

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
      "ws2_32",
      "dbghelp"
   }

   -- TODO: spirv-cross reflection disabled — Vulkan SDK pre-built libs have ABI mismatch.
   -- When spirv-cross is built from source, add: links { "spirv-cross-core", "spirv-cross-glsl" }

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
      