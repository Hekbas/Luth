-- Offscreen capture harness. Links Luth only: no Luthien, no ImGui context, no editor hooks. This is
-- the first target that exercises the EditorHooks decoupling instead of merely asserting it, so a link
-- error here means an engine->editor dependency leaked back in.
project "LuthShot"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++20"

   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
   objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   buildoptions { "/utf-8" }

   defines
   {
      "GLFW_INCLUDE_NONE",
      "FMT_HEADER_ONLY=1"
   }

   files
   {
      "**.h",
      "**.cpp"
   }

   includedirs
   {
      ".",
      "%{wks.location}/luth/source",
      "%{wks.location}/luth/extern/source",
      "%{wks.location}/luth/extern/config-headers",
      IncludeDir["assimp"],
      IncludeDir["glfw"],
      IncludeDir["glm"],
      IncludeDir["imgui"],
      IncludeDir["spdlog"],
      IncludeDir["tracy"],
      IncludeDir["vulkan"],
      IncludeDir["jolt"]
   }

   libdirs { LibraryDir["vulkan"] }

   -- Slang is the engine's only shader backend and compiles in-process at load, so the DLLs have to sit
   -- next to this exe too; slang-compiler.dll LoadLibrary's its siblings on demand.
   postbuildcommands
   {
      "{COPY} " .. LibraryDir["vulkan"] .. "/slang-compiler.dll %{cfg.targetdir}",
      "{COPY} " .. LibraryDir["vulkan"] .. "/slang-glslang.dll %{cfg.targetdir}",
      "{COPY} " .. LibraryDir["vulkan"] .. "/slang-glsl-module.dll %{cfg.targetdir}",
      "{COPY} " .. LibraryDir["vulkan"] .. "/slang-rt.dll %{cfg.targetdir}"
   }

   links
   {
      "Luth",
      "vulkan-1"
   }

   filter "configurations:Debug"
      defines { "LUTH_BUILD_DEBUG", "TRACY_ENABLE", "TRACY_FIBERS", "TRACY_ON_DEMAND" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "LUTH_BUILD_RELEASE", "TRACY_ENABLE", "TRACY_FIBERS", "TRACY_ON_DEMAND" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      defines { "LUTH_BUILD_DIST" }
      runtime "Release"
      optimize "on"

   filter "configurations:DebugASan"
      defines { "LUTH_BUILD_DEBUG" }
      runtime "Release"
      symbols "on"
      editandcontinue "Off"
      buildoptions { "/fsanitize=address" }

   filter {}
