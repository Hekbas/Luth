#pragma once

// Enable Tracy in Debug/Release, disable in Dist
#if defined(TRACY_ENABLE)
    #include <tracy/Tracy.hpp>

    #define LH_PROFILE_FRAME(name)          FrameMarkNamed(name)
    #define LH_PROFILE_FUNCTION()           ZoneScoped
    #define LH_PROFILE_SCOPE(name)          ZoneScopedN(name)
    #define LH_PROFILE_SCOPE_DYNAMIC(name)  ZoneScoped; ZoneName(name.c_str(), name.size())
    #define LH_PROFILE_TAG(key, val)        ZoneText(val, strlen(val))
    #define LH_PROFILE_ALLOC(ptr, size)     TracyAlloc(ptr, size)
    #define LH_PROFILE_FREE(ptr)            TracyFree(ptr)
    #define LH_PROFILE_THREAD(name)         tracy::SetThreadName(name)
    #define LH_PROFILE_FIBER_ENTER(name)    TracyFiberEnter(name)
    #define LH_PROFILE_FIBER_LEAVE          TracyFiberLeave
#else
    #define LH_PROFILE_FRAME(name)
    #define LH_PROFILE_FUNCTION()
    #define LH_PROFILE_SCOPE(name)
    #define LH_PROFILE_SCOPE_DYNAMIC(name)
    #define LH_PROFILE_TAG(key, val)
    #define LH_PROFILE_ALLOC(ptr, size)
    #define LH_PROFILE_FREE(ptr)
    #define LH_PROFILE_THREAD(name)
    #define LH_PROFILE_FIBER_ENTER(name)
    #define LH_PROFILE_FIBER_LEAVE
#endif
