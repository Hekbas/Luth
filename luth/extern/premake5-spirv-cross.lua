project "spirv-cross"
   kind "StaticLib"
   language "C++"
   cppdialect "C++20"
   architecture "x86_64"

   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
   objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files {
      "source/spirv-cross/spirv_cross.cpp",
      "source/spirv-cross/spirv_cfg.cpp",
      "source/spirv-cross/spirv_cross_parsed_ir.cpp",
      "source/spirv-cross/spirv_parser.cpp",
      "source/spirv-cross/spirv_glsl.cpp",
      "source/spirv-cross/spirv_cross_util.cpp",
   }

   includedirs { "source/spirv-cross" }

   buildoptions { "/utf-8" }
   defines { "SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS" }

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"
   filter "configurations:Release"
      runtime "Release"
      optimize "on"
   filter "configurations:Dist"
      runtime "Release"
      optimize "on"
   filter {}
