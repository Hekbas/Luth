#pragma once

// LH_PROFILE_* macros that compile to Tracy zones (Debug, Release builds with TRACY_ENABLE) or
// to no-ops (Dist). LH_PROFILE_FRAME / FUNCTION / SCOPE are the common entry points; allocator
// macros plumb LH_NEW / LH_ALLOC sites into Tracy's memory tab. Fiber macros depend on the
// optional TRACY_FIBERS define so call sites can stay clean either way.

#if defined(TRACY_ENABLE)
    #include <tracy/Tracy.hpp>

    #define LH_PROFILE_FRAME(name)             FrameMarkNamed(name)
    #define LH_PROFILE_FUNCTION()              ZoneScoped
    #define LH_PROFILE_SCOPE(name)             ZoneScopedN(name)
    #define LH_PROFILE_SCOPE_DYNAMIC(name)     ZoneScoped; ZoneName(name.c_str(), name.size())
    #define LH_PROFILE_SCOPE_DYNAMIC_CSTR(n)   ZoneScoped; ZoneName(n, strlen(n))
    #define LH_PROFILE_TAG(key, val)           ZoneText(val, strlen(val))
    #define LH_PROFILE_ALLOC(ptr, size)     TracyAlloc(ptr, size)
    #define LH_PROFILE_FREE(ptr)            TracyFree(ptr)
    #define LH_PROFILE_THREAD(name)         tracy::SetThreadName(name)

    // Numeric time-series plots + discrete events. GPU zone macros live in renderer/backend/vulkan/GpuTracy.h.
    #define LH_PROFILE_PLOT(name, val)         TracyPlot(name, val)
    #define LH_PLOT_FORMAT_NUMBER              tracy::PlotFormatType::Number
    #define LH_PLOT_FORMAT_MEMORY              tracy::PlotFormatType::Memory
    #define LH_PLOT_FORMAT_PERCENTAGE          tracy::PlotFormatType::Percentage
    #define LH_PROFILE_PLOT_CONFIG(name, fmt, step, fill, color) TracyPlotConfig(name, fmt, step, fill, color)
    #define LH_PROFILE_MESSAGE(txt)            TracyMessage(txt, strlen(txt))
    #define LH_PROFILE_MESSAGE_COLOR(txt, col) TracyMessageC(txt, strlen(txt), col)

    // No-op when TRACY_FIBERS is not defined, so call sites stay clean.
    #if defined(TRACY_FIBERS)
        #define LH_PROFILE_FIBER_ENTER(name)    TracyFiberEnter(name)
        #define LH_PROFILE_FIBER_LEAVE          TracyFiberLeave
    #else
        #define LH_PROFILE_FIBER_ENTER(name)    ((void)0)
        #define LH_PROFILE_FIBER_LEAVE          ((void)0)
    #endif
#else
    #define LH_PROFILE_FRAME(name)
    #define LH_PROFILE_FUNCTION()
    #define LH_PROFILE_SCOPE(name)
    #define LH_PROFILE_SCOPE_DYNAMIC(name)
    #define LH_PROFILE_SCOPE_DYNAMIC_CSTR(n)
    #define LH_PROFILE_TAG(key, val)
    #define LH_PROFILE_ALLOC(ptr, size)
    #define LH_PROFILE_FREE(ptr)
    #define LH_PROFILE_THREAD(name)
    #define LH_PROFILE_PLOT(name, val)
    #define LH_PLOT_FORMAT_NUMBER              0
    #define LH_PLOT_FORMAT_MEMORY              0
    #define LH_PLOT_FORMAT_PERCENTAGE          0
    #define LH_PROFILE_PLOT_CONFIG(name, fmt, step, fill, color)
    #define LH_PROFILE_MESSAGE(txt)
    #define LH_PROFILE_MESSAGE_COLOR(txt, col)
    #define LH_PROFILE_FIBER_ENTER(name)
    #define LH_PROFILE_FIBER_LEAVE
#endif
