project "JobSysProof"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++20"

   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
   objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   buildoptions { "/utf-8" }

   -- Mirror Luth/Jolt instruction-set defines so Jolt headers compile
   -- consistently with the lib (ABI mismatch otherwise).
   defines
   {
      "JPH_USE_AVX2",
      "JPH_USE_AVX",
      "JPH_USE_SSE4_1",
      "JPH_USE_SSE4_2",
      "JPH_USE_FMADD",
      "JPH_USE_F16C",
      "JPH_USE_LZCNT",
      "JPH_USE_TZCNT"
   }

   files { "**.cpp", "**.h" }

   includedirs
   {
      "%{wks.location}/luth/source",
      "%{wks.location}/luth/extern/source",
      "%{wks.location}/luth/extern/config-headers",
      IncludeDir["glm"],
      IncludeDir["spdlog"],
      IncludeDir["tracy"],
      IncludeDir["vulkan"],
      IncludeDir["jolt"]
   }

   libdirs
   {
      LibraryDir["vulkan"]
   }

   links
   {
      "Luth",
      "Jolt",
      "Tracy",
      "vulkan-1",
      "ws2_32",
      "dbghelp"
   }

   filter "configurations:Debug"
      defines { "LUTH_BUILD_DEBUG", "TRACY_ENABLE", "TRACY_FIBERS", "TRACY_ON_DEMAND",
                "JPH_ENABLE_ASSERTS", "JPH_DEBUG_RENDERER" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "LUTH_BUILD_RELEASE", "TRACY_ENABLE", "TRACY_FIBERS", "TRACY_ON_DEMAND",
                "JPH_DEBUG_RENDERER", "NDEBUG" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      defines { "LUTH_BUILD_DIST", "NDEBUG" }
      runtime "Release"
      optimize "on"

   -- DebugASan: matches Luth's filter shape. TRACY_ENABLE omitted to avoid the dbghelp
   -- static-init flake under ASan (see luth/extern/premake5-tracy.lua).
   filter "configurations:DebugASan"
      defines { "LUTH_BUILD_DEBUG", "JPH_ENABLE_ASSERTS", "JPH_DEBUG_RENDERER" }
      runtime "Release"
      symbols "on"
      editandcontinue "Off"
      buildoptions { "/fsanitize=address" }

   filter {}
