project "Tracy"
   kind "StaticLib"
   language "C++"
   cppdialect "C++20"
   architecture "x86_64"
   
   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
   objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")
   
   files { "source/tracy/public/TracyClient.cpp" }
   includedirs { "source/tracy/public" }
   
   filter "configurations:Debug"
      defines { "TRACY_ENABLE", "TRACY_FIBERS", "TRACY_ON_DEMAND" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "TRACY_ENABLE", "TRACY_FIBERS", "TRACY_ON_DEMAND" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      runtime "Release"
      optimize "on"
      -- Tracy is disabled in Dist, effectively compiling an empty lib

   -- DebugASan: Tracy is compiled OUT (no TRACY_ENABLE define). Tracy's static-init
   -- spawns a SymbolWorker that initializes Windows dbghelp.dll, which trips an ASan
   -- strlen interceptor false positive intermittently (~20% flake on V1b stress).
   -- We're sanitizing here, not profiling; Tracy.lib becomes effectively empty.
   filter "configurations:DebugASan"
      runtime "Release"
      symbols "on"

   filter {}
   