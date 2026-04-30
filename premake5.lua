include "dependencies.lua"

workspace "Luth"
   architecture "x86_64"
   startproject "Runtime"

   filter "toolset:msc*"
      buildoptions { "/utf-8" }
   filter {}

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
   include "luthien"
   include "runtime"
group ""


group "Tools"
   include "extern/premake"
group ""
