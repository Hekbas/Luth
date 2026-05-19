project "Jolt"
   kind "StaticLib"
   language "C++"
   cppdialect "C++17"
   architecture "x86_64"
   warnings "Off"

   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
   objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files
   {
      "source/jolt/Jolt/**.cpp",
      "source/jolt/Jolt/**.h",
      "source/jolt/Jolt/**.inl",
      "source/jolt/Jolt/Jolt.natvis"
   }

   removefiles
   {
      "source/jolt/Jolt/ObjectStream/ObjectStream.cpp",
      "source/jolt/Jolt/ObjectStream/ObjectStreamBinaryIn.cpp",
      "source/jolt/Jolt/ObjectStream/ObjectStreamBinaryIn.h",
      "source/jolt/Jolt/ObjectStream/ObjectStreamBinaryOut.cpp",
      "source/jolt/Jolt/ObjectStream/ObjectStreamBinaryOut.h",
      "source/jolt/Jolt/ObjectStream/ObjectStreamIn.cpp",
      "source/jolt/Jolt/ObjectStream/ObjectStreamIn.h",
      "source/jolt/Jolt/ObjectStream/ObjectStreamOut.cpp",
      "source/jolt/Jolt/ObjectStream/ObjectStreamOut.h",
      "source/jolt/Jolt/ObjectStream/ObjectStreamTextIn.cpp",
      "source/jolt/Jolt/ObjectStream/ObjectStreamTextIn.h",
      "source/jolt/Jolt/ObjectStream/ObjectStreamTextOut.cpp",
      "source/jolt/Jolt/ObjectStream/ObjectStreamTextOut.h",
      "source/jolt/Jolt/ObjectStream/ObjectStreamTypes.h",
      "source/jolt/Jolt/ObjectStream/GetPrimitiveTypeOfType.h",
      "source/jolt/Jolt/ObjectStream/TypeDeclarations.cpp"
   }

   includedirs { "source/jolt" }

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

   filter "system:windows"
      vectorextensions "AVX2"
      buildoptions { "/utf-8" }

   filter "configurations:Debug"
      defines { "JPH_ENABLE_ASSERTS", "JPH_DEBUG_RENDERER", "_DEBUG" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "NDEBUG", "JPH_DEBUG_RENDERER" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      defines { "NDEBUG" }
      runtime "Release"
      optimize "on"

   -- DebugASan: same defines + CRT as Release so Jolt.lib's CRT matches Luth.lib's
   -- DebugASan (Release CRT). JPH_DEBUG_RENDERER kept for debug overlay. Not
   -- /fsanitize=address (only Luth + its consumers are instrumented).
   filter "configurations:DebugASan"
      defines { "JPH_DEBUG_RENDERER", "JPH_ENABLE_ASSERTS" }
      runtime "Release"
      symbols "on"

   filter {}
