#pragma once

// LH_PROFILE_GPU_* — Tracy Vulkan GPU-zone macros. Kept out of core Profiler.h so <vulkan/vulkan.h>
// and TracyVulkan.hpp stay scoped to the renderer; include only where Vulkan is already in scope.
// GpuTracyCtx is one persistent context per queue (graphics + async-compute); collect once per frame.

#if defined(TRACY_ENABLE)
    #include <vulkan/vulkan.h>
    #include <tracy/TracyVulkan.hpp>

    namespace Luth { using GpuTracyCtx = TracyVkCtx; }

    #define LH_PROFILE_GPU_CONTEXT(physDev, dev, queue, cmd)   TracyVkContext(physDev, dev, queue, cmd)
    #define LH_PROFILE_GPU_CONTEXT_NAME(ctx, name, len)        TracyVkContextName(ctx, name, len)
    #define LH_PROFILE_GPU_DESTROY(ctx)                        TracyVkDestroy(ctx)
    #define LH_PROFILE_GPU_COLLECT(ctx, cmd)                   TracyVkCollect(ctx, cmd)
    #define LH_PROFILE_GPU_ZONE(ctx, cmd, name)                TracyVkZone(ctx, cmd, name)
    #define LH_PROFILE_GPU_ZONE_TRANSIENT(var, ctx, cmd, name) TracyVkZoneTransient(ctx, var, cmd, name, true)
#else
    namespace Luth { using GpuTracyCtx = void*; }

    #define LH_PROFILE_GPU_CONTEXT(physDev, dev, queue, cmd)   nullptr
    #define LH_PROFILE_GPU_CONTEXT_NAME(ctx, name, len)        ((void)0)
    #define LH_PROFILE_GPU_DESTROY(ctx)                        ((void)0)
    #define LH_PROFILE_GPU_COLLECT(ctx, cmd)                   ((void)0)
    #define LH_PROFILE_GPU_ZONE(ctx, cmd, name)                ((void)0)
    #define LH_PROFILE_GPU_ZONE_TRANSIENT(var, ctx, cmd, name) ((void)0)
#endif
