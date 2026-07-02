#pragma once

// ---- Build configuration ----
// Exactly one of these is defined by premake5.lua per configuration. Engine code never tests
// _DEBUG / NDEBUG; those are toolchain artifacts (MSVC's /MDd vs /MD; not portable across
// compilers or premake versions).

#if !defined(LUTH_BUILD_DEBUG) && !defined(LUTH_BUILD_RELEASE) && !defined(LUTH_BUILD_DIST)
    #error "No LUTH_BUILD_* configuration defined — premake5.lua must set one"
#endif

// ---- Derived feature flags ----
// Default policy by build config; each may be overridden on the command line with
// -DLUTH_ENABLE_X=0|1 (premake `defines { "LUTH_ENABLE_X=1" }`).

// Vulkan validation layers + VK_EXT_debug_utils messenger.
#if !defined(LUTH_ENABLE_VALIDATION)
    #if defined(LUTH_BUILD_DEBUG)
        #define LUTH_ENABLE_VALIDATION 1
    #else
        #define LUTH_ENABLE_VALIDATION 0
    #endif
#endif

// SPIRV-Cross shader reflection. Currently always-on; centralized here so a future Dist config
// can drop reflection without touching call sites.
#if !defined(LUTH_SPIRV_CROSS_ENABLED)
    #define LUTH_SPIRV_CROSS_ENABLED 1
#endif
