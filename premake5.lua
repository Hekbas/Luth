include "dependencies.lua"

workspace "Luth"
   architecture "x86_64"
   startproject "Luthien"

   buildoptions { "/utf-8" }

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
   include "luth"
group ""

group "Luth/Extern"
      include "luth/extern/premake5-assimp"
      include "luth/extern/premake5-glfw"
      include "luth/extern/premake5-glm"
      include "luth/extern/premake5-imgui"
      include "luth/extern/premake5-imguizmo"
      include "luth/extern/premake5-tracy"
      include "luth/extern/premake5-spirv-cross"
group ""

group "Luthien"
   include "Luthien"
group ""


group "Tools"
   include "extern/premake"
group ""
