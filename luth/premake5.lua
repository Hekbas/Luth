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
      "SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS",
      -- Jolt instruction-set defines (must mirror luth/extern/premake5-jolt.lua so
      -- Jolt headers compile consistently across the lib and its consumers)
      "JPH_USE_AVX2",
      "JPH_USE_AVX",
      "JPH_USE_SSE4_1",
      "JPH_USE_SSE4_2",
      "JPH_USE_FMADD",
      "JPH_USE_F16C",
      "JPH_USE_LZCNT",
      "JPH_USE_TZCNT"
   }

   files
   {
      "source/**.h",
      "source/**.cpp",
      "source/**.asm",
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
      IncludeDir["spirv_cross"],
      IncludeDir["jolt"]
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
      "Jolt",
      "ws2_32",
      "dbghelp"
   }

   filter "configurations:Debug"
      defines { "LUTH_BUILD_DEBUG", "TRACY_ENABLE", "TRACY_ON_DEMAND",
                "JPH_ENABLE_ASSERTS", "JPH_DEBUG_RENDERER" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "LUTH_BUILD_RELEASE", "TRACY_ENABLE", "TRACY_ON_DEMAND",
                "JPH_DEBUG_RENDERER", "NDEBUG" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      defines { "LUTH_BUILD_DIST", "NDEBUG" }
      runtime "Release"
      optimize "on"

   -- DebugASan: debug-style defines + Release CRT + /fsanitize=address. MSVC's Debug
   -- CRT is incompatible with ASan; Release CRT is mandatory. Requires MSVC 16.9+.
   -- TRACY_ENABLE intentionally omitted — Tracy's static-init dbghelp thread trips an
   -- ASan strlen false positive (see premake5-tracy.lua).
   filter "configurations:DebugASan"
      defines { "LUTH_BUILD_DEBUG", "JPH_ENABLE_ASSERTS", "JPH_DEBUG_RENDERER" }
      runtime "Release"
      symbols "on"
      editandcontinue "Off"
      buildoptions { "/fsanitize=address" }
