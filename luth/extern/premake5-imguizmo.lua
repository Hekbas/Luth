project "imguizmo"
   kind "StaticLib"
   language "C++"
   cppdialect "C++20"

   targetdir ("%{wks.location}/bin/" .. outputdir .. "/%{prj.name}")
   objdir ("%{wks.location}/bin-int/" .. outputdir .. "/%{prj.name}")

   files
   {
      "source/imguizmo/ImGuizmo.h",
      "source/imguizmo/ImGuizmo.cpp"
   }

   includedirs
   {
      "source/imguizmo",
      IncludeDir["imgui"]
   }

   defines
   {
      "IMGUI_DEFINE_MATH_OPERATORS"
   }

   filter "configurations:Debug"
      runtime "Debug"
      symbols "on"

   filter "configurations:Release"
      runtime "Release"
      optimize "on"

   filter "configurations:Dist"
      runtime "Release"
      optimize "on"
