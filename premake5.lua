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
      include "luth/extern/premake5-assimp"
      include "luth/extern/premake5-glad"
      include "luth/extern/premake5-glfw"
      include "luth/extern/premake5-glm"
      include "luth/extern/premake5-imgui"
group ""

group "Luthien"
   include "Luthien"
group ""

group "Luthien/Extern"
   include "luthien/extern"
group ""

group "Sandbox"
   include "Sandbox"
group ""

group "Tools"
   include "extern/premake"
group ""
