workspace "Luth"
   architecture "x86_64"
   startproject "Luth"

   configurations
   {
      "Debug",
      "Release",
      "Dist"
   }

outputdir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

dofile("luth/premake5.lua");
dofile("core/premake5.lua");