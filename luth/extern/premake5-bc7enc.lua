project "bc7enc"
   kind "StaticLib"
   language "C++"
   cppdialect "C++20"

   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
   objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   -- Minimal BCn block-encoder subset of richgel999/bc7enc_rdo: bc7enc (BC7) + rgbcx (BC1/3/4/5).
   -- The RDO/ERT, lodepng, miniz and CLI harness are intentionally NOT vendored -- the core block
   -- encoders are self-contained (neither includes utils.h).
   files
   {
      "source/bc7enc/bc7enc.h",
      "source/bc7enc/bc7enc.cpp",
      "source/bc7enc/rgbcx.h",
      "source/bc7enc/rgbcx.cpp",
      "source/bc7enc/rgbcx_table4.h",
      "source/bc7enc/rgbcx_table4_small.h"
   }

   includedirs
   {
      "source/bc7enc"
   }

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"
      -- Import-tool code, never single-stepped. Unoptimized BC7 turns a cold Bistro import into a
      -- coffee break, so optimize even in Debug (the encode result is bit-identical to Release).
      optimize "Speed"

   filter "configurations:Release"
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      runtime "Release"
      optimize "on"

   filter "configurations:DebugASan"
      runtime "Release"
      symbols "on"
      optimize "Speed"
