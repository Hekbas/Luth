include "dependencies.lua"

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


group "Luth"
   include "Luth"
group ""

group "Luth/Extern"
      include "luth/extern"
group ""

group "Luthien"
   include "Luthien"
group ""

group "Luthien/Extern"
      include "luthien/extern"
group ""