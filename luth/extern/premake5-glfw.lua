project "glfw"
	kind "StaticLib"
	language "C"
	staticruntime "off"
	warnings "off"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"source/glfw/include/GLFW/glfw3.h",
		"source/glfw/include/GLFW/glfw3native.h",

		"source/glfw/src/glfw_config.h",
		"source/glfw/src/context.c",
		"source/glfw/src/init.c",
		"source/glfw/src/input.c",
		"source/glfw/src/monitor.c",
		"source/glfw/src/vulkan.c",
		"source/glfw/src/window.c",

		"source/glfw/src/internal.h",
		"source/glfw/src/platform.h",
		"source/glfw/src/platform.c",
		"source/glfw/src/mappings.h",
		"source/glfw/src/egl_context.c",
		"source/glfw/src/osmesa_context.c",

		"source/glfw/src/null_init.c",
		"source/glfw/src/null_joystick.h",
		"source/glfw/src/null_joystick.c",
		"source/glfw/src/null_monitor.c",
		"source/glfw/src/null_platform.h",
		"source/glfw/src/null_window.c"
	}

	filter "system:windows"
		systemversion "latest"

		files
		{
			"source/glfw/src/win32_init.c",
			"source/glfw/src/win32_joystick.c",
			"source/glfw/src/win32_module.c",
			"source/glfw/src/win32_monitor.c",
			"source/glfw/src/win32_time.h",
			"source/glfw/src/win32_time.c",
			"source/glfw/src/win32_thread.h",
			"source/glfw/src/win32_thread.c",
			"source/glfw/src/win32_window.c",
			"source/glfw/src/wgl_context.c",
			"source/glfw/src/egl_context.c",
			"source/glfw/src/osmesa_context.c"
		}

		defines 
		{ 
			"_GLFW_WIN32",
			"_CRT_SECURE_NO_WARNINGS"
		}

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
