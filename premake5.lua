workspace "Luth"
   architecture "x86_64"
   startproject "Luthien"

   configurations
   {
      "Debug",
      "Release",
      "Dist"
   }

   flags
	{
		"MultiProcessorCompile"
	}

outputdir = "%{cfg.system}-%{cfg.architecture}/%{cfg.buildcfg}"

include "Luth"
include "Luthien"