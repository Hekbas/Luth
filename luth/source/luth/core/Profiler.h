#pragma once

// TODO: Integrate Tracy Profiler here
// For now, these are empty macros to prepare the codebase.

#if 0 // Set to 1 when Tracy is included
    #include <tracy/Tracy.hpp>
    #define LH_PROFILE_FRAME(name)      FrameMarkNamed(name)
    #define LH_PROFILE_FUNCTION()       ZoneScoped
    #define LH_PROFILE_SCOPE(name)      ZoneScopedN(name)
    #define LH_PROFILE_TAG(key, val)    ZoneText(val, strlen(val))
#else
    #define LH_PROFILE_FRAME(name)
    #define LH_PROFILE_FUNCTION()
    #define LH_PROFILE_SCOPE(name)
    #define LH_PROFILE_TAG(key, val)
#endif
