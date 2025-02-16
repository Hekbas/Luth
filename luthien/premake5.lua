project "Luthien"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++20"
   
   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   buildoptions { "/utf-8" }

   files
   {
      "source/**.h",
      "source/**.cpp"
   }

   includedirs
   {
      "source",
      "%{wks.location}/luth/source",
      IncludeDir["glfw"],
      IncludeDir["glm"],
      IncludeDir["spdlog"]
   }

   links
   {
      "Luth",
      "ImGui"
   }

   filter "configurations:Debug"
      defines { "DEBUG" }
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      defines { "RELEASE" }
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      defines { "DIST" }
      runtime "Release"
      optimize "on"
      