project "Sandbox"
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
      "source/**.h",
      "source/**.cpp"
   }

   includedirs
   {
      "source",
      "%{wks.location}/luth/source",
      "%{wks.location}/luth/extern/source",
      "%{wks.location}/luth/extern/config-headers",
      IncludeDir["assimp"],
      IncludeDir["glad"],
      IncludeDir["glfw"],
      IncludeDir["glm"],
      IncludeDir["imgui"],
      IncludeDir["spdlog"],
      IncludeDir["tracy"],
      IncludeDir["vulkan"]
   }

   libdirs
   {
      LibraryDir["vulkan"]
   }

   postbuildcommands
   {
      "{COPY} " .. LibraryDir["vulkan"] .. "/shaderc_shared.dll %{cfg.targetdir}"
   }

   links
   {
      "Luth",
      "vulkan-1",
      "shaderc_shared"
   }

   filter "configurations:Debug"
      defines { "DEBUG", "TRACY_ENABLE" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "RELEASE", "TRACY_ENABLE" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      defines { "DIST" }
      runtime "Release"
      optimize "on"
      