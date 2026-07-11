include "dependencies.lua"

workspace "Luth"
   architecture "x86_64"
   startproject "Runtime"

   buildoptions { "/utf-8" }

   configurations
   {
      "Debug",
      "DebugASan",  -- ASan-instrumented Debug. Requires MSVC 16.9+. Release CRT mandatory.
      "Release",
      "Dist"
   }

   flags
	{
		"MultiProcessorCompile"
	}

   -- MSVC ASan emits container-annotation symbols into every TU that uses std::string/vector.
   -- Mixed ASan/non-ASan static libs trip LNK2038 annotate_string/vector mismatches; defining
   -- _DISABLE_*_ANNOTATION workspace-wide for DebugASan unifies the symbols across all TUs.
   -- Heap/stack UAF detection (the actual point) is preserved; only container bounds-checking
   -- is forfeited.
   filter "configurations:DebugASan"
      defines { "_DISABLE_VECTOR_ANNOTATION", "_DISABLE_STRING_ANNOTATION" }
   filter {}

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
      include "luth/extern/premake5-jolt"
      include "luth/extern/premake5-bc7enc"
group ""

group "Luthien"
   include "luthien"
   include "runtime"
group ""

group "Samples"
   include "samples/physics_smoke"
group ""

group "Tests"
   include "tests"
group ""


group "Tools"
   include "extern/premake"
group ""
