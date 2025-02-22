project "imgui"
	kind "StaticLib"
	language "C++"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		-- Core ImGui
		"source/imgui/imconfig.h",
		"source/imgui/imgui.h",
		"source/imgui/imgui.cpp",
		"source/imgui/imgui_demo.cpp",
		"source/imgui/imgui_draw.cpp",
		"source/imgui/imgui_internal.h",
		"source/imgui/imstb_rectpack.h",
		"source/imgui/imgui_tables.cpp",
		"source/imgui/imstb_textedit.h",
		"source/imgui/imstb_truetype.h",
		"source/imgui/imgui_widgets.cpp",
		-- Backend GLFW + OpenGL
		"source/imgui/backends/imgui_impl_glfw.h",
		"source/imgui/backends/imgui_impl_glfw.cpp",
		"source/imgui/backends/imgui_impl_opengl3.h",
		"source/imgui/backends/imgui_impl_opengl3.cpp"
	}

	includedirs
	{
		"source",
		"source/imgui",
		IncludeDir["glfw"]
	}

	filter "system:windows"
		systemversion "latest"
		cppdialect "C++17"

	filter "system:linux"
		pic "On"
		systemversion "latest"
		cppdialect "C++17"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
	