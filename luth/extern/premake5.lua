project "assimp"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	warnings "Off"
	optimize "Speed"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		-- Dependencies
		"source/assimp/contrib/unzip/**",
		"source/assimp/contrib/irrXML/**",
		"source/assimp/contrib/zlib/*",
		-- "source/assimp/contrib/poly2tri/poly2tri/**",

		-- Common
		"source/assimp/code/Common/**",
		"source/assimp/code/PostProcessing/**",
		"source/assimp/code/Material/**",
		"source/assimp/code/CApi/**",
		"source/assimp/code/Geometry/**",

		-- Importers
		"source/assimp/code/AssetLib/IQM/**",
		"source/assimp/code/AssetLib/Assbin/**",
		"source/assimp/code/AssetLib/Collada/**",
		"source/assimp/code/AssetLib/Obj/**",
		-- "source/assimp/code/AssetLib/Blender/**",
		"source/assimp/code/AssetLib/FBX/**",
		-- "source/assimp/code/glTF2/**",
		-- "source/assimp/code/glTF/**",
		"source/assimp/code/Assbin/**" -- Very fast format to parse/write, useful for caching
	}

	includedirs
	{
		"config-headers/",
		"config-headers/assimp/",
		"source/assimp/",
		"source/assimp/contrib/",
		"source/assimp/contrib/irrXML/",
		"source/assimp/contrib/unzip/",
		"source/assimp/contrib/rapidjson/include/",
		"source/assimp/contrib/pugixml/src/",
		"source/assimp/contrib/zlib/",
		"source/assimp/contrib/utf8cpp/source",
		"source/assimp/code",
		"source/assimp/include"
	}
	
	defines
	{
		"ASSIMP_BUILD_NO_OWN_ZLIB",

		-- Importers
		"ASSIMP_BUILD_NO_3D_IMPORTER",
		"ASSIMP_BUILD_NO_3DS_IMPORTER",
		"ASSIMP_BUILD_NO_3MF_IMPORTER",
		"ASSIMP_BUILD_NO_AC_IMPORTER",
		"ASSIMP_BUILD_NO_AMF_IMPORTER",
		"ASSIMP_BUILD_NO_ASE_IMPORTER",
		-- "ASSIMP_BUILD_NO_ASSBIN_IMPORTER",
		"ASSIMP_BUILD_NO_B3D_IMPORTER",
		"ASSIMP_BUILD_NO_BLEND_IMPORTER",
		"ASSIMP_BUILD_NO_BVH_IMPORTER",
		"ASSIMP_BUILD_NO_C4D_IMPORTER",
		"ASSIMP_BUILD_NO_COB_IMPORTER",
		-- "ASSIMP_BUILD_NO_COLLADA_IMPORTER",
		"ASSIMP_BUILD_NO_CSM_IMPORTER",
		"ASSIMP_BUILD_NO_DXF_IMPORTER",
		-- "ASSIMP_BUILD_NO_FBX_IMPORTER",
		"ASSIMP_BUILD_NO_GLTF_IMPORTER",
		"ASSIMP_BUILD_NO_HMP_IMPORTER",
		"ASSIMP_BUILD_NO_IFC_IMPORTER",
		"ASSIMP_BUILD_NO_IRR_IMPORTER",
		"ASSIMP_BUILD_NO_IRRMESH_IMPORTER",
		"ASSIMP_BUILD_NO_LWO_IMPORTER",
		"ASSIMP_BUILD_NO_LWS_IMPORTER",
		"ASSIMP_BUILD_NO_M3D_IMPORTER",
		"ASSIMP_BUILD_NO_MD2_IMPORTER",
		"ASSIMP_BUILD_NO_MD3_IMPORTER",
		"ASSIMP_BUILD_NO_MD5_IMPORTER",
		"ASSIMP_BUILD_NO_MDC_IMPORTER",
		"ASSIMP_BUILD_NO_MDL_IMPORTER",
		"ASSIMP_BUILD_NO_MMD_IMPORTER",
		"ASSIMP_BUILD_NO_MS3D_IMPORTER",
		"ASSIMP_BUILD_NO_NDO_IMPORTER",
		"ASSIMP_BUILD_NO_NFF_IMPORTER",
		-- "ASSIMP_BUILD_NO_OBJ_IMPORTER",
		"ASSIMP_BUILD_NO_OFF_IMPORTER",
		"ASSIMP_BUILD_NO_OGRE_IMPORTER",
		"ASSIMP_BUILD_NO_OPENGEX_IMPORTER",
		"ASSIMP_BUILD_NO_PBRT_IMPORTER",
		"ASSIMP_BUILD_NO_PLY_IMPORTER",
		"ASSIMP_BUILD_NO_Q3BSP_IMPORTER",
		"ASSIMP_BUILD_NO_Q3D_IMPORTER",
		"ASSIMP_BUILD_NO_RAW_IMPORTER",
		"ASSIMP_BUILD_NO_SIB_IMPORTER",
		"ASSIMP_BUILD_NO_SMD_IMPORTER",
		"ASSIMP_BUILD_NO_STEP_IMPORTER",
		"ASSIMP_BUILD_NO_STL_IMPORTER",
		"ASSIMP_BUILD_NO_TERRAGEN_IMPORTER",
		"ASSIMP_BUILD_NO_USD_IMPORTER",
		"ASSIMP_BUILD_NO_X_IMPORTER",
		"ASSIMP_BUILD_NO_X3D_IMPORTER",
		"ASSIMP_BUILD_NO_XGL_IMPORTER",

		-- Exporters
		"ASSIMP_BUILD_NO_3DS_EXPORTER",
		"ASSIMP_BUILD_NO_3MF_EXPORTER",
		-- "ASSIMP_BUILD_NO_ASSBIN_EXPORTER",
		"ASSIMP_BUILD_NO_ASSJSON_EXPORTER",
		"ASSIMP_BUILD_NO_ASSXML_EXPORTER",
		"ASSIMP_BUILD_NO_COLLADA_EXPORTER",
		"ASSIMP_BUILD_NO_FBX_EXPORTER",
		"ASSIMP_BUILD_NO_GLTF_EXPORTER",
		"ASSIMP_BUILD_NO_M3D_EXPORTER",
		"ASSIMP_BUILD_NO_OBJ_EXPORTER",
		"ASSIMP_BUILD_NO_PBRT_EXPORTER",
		"ASSIMP_BUILD_NO_PLY_EXPORTER",
		"ASSIMP_BUILD_NO_STEP_EXPORTER",
		"ASSIMP_BUILD_NO_STL_EXPORTER",
		"ASSIMP_BUILD_NO_USD_EXPORTER",
		"ASSIMP_BUILD_NO_X3D_EXPORTER",
		"ASSIMP_BUILD_NO_X_EXPORTER"
	}

	filter "system:windows"
		systemversion "latest"

	filter  "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter  "configurations:Release"
		runtime "Release"
		optimize "on"
	
----------------------------------------------------------------------------------------------

project "glad"
	kind "StaticLib"
	language "C"
	staticruntime "off"
	
	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

	files
	{
		"source/glad/include/glad/glad.h",
		"source/glad/include/KHR/khrplatform.h",
		"source/glad/src/glad.c"
	}

	includedirs
	{
		"source/glad/include"
	}
	
	filter "system:windows"
		systemversion "latest"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"

----------------------------------------------------------------------------------------------

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

----------------------------------------------------------------------------------------------

project "glm"
	kind "StaticLib"
	language "C"
	cppdialect "C++20"
	architecture "x86_64"

	targetdir ("bin/" .. outputdir .. "/%{prj.name}")
	objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
	
	includedirs { "source/glm" } 

	files
	{
		"source/glm/glm/**"
	}
	
	filter "system:windows"
		systemversion "latest"
		staticruntime "On"

		defines 
		{ 
			"_GLM_WIN32",
			"_CRT_SECURE_NO_WARNINGS"
		}

	filter "system:linux"
		pic "On"

		systemversion "latest"
		staticruntime "On"

		defines
		{
			"_GLM_X11"
		}	

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
