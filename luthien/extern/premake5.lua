project "ImGui"
	kind "StaticLib"
	language "C++"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"source/imgui/imconfig.h",
		"source/imgui/imgui.h",
		"source/imgui/imgui.cpp",
		"source/imgui/imgui_draw.cpp",
		"source/imgui/imgui_internal.h",
		"source/imgui/imgui_widgets.cpp",
		"source/imgui/imstb_rectpack.h",
		"source/imgui/imstb_textedit.h",
		"source/imgui/imstb_truetype.h",
		"source/imgui/imgui_demo.cpp"
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
