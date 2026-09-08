#pragma once

#include "luth/core/types/LuthTypes.h"

#include <filesystem>

namespace Luth
{
    // Offscreen capture request parsed from argv by App::ParseCommandLineArgs. Active only when both
    // scenePath and shotPath are set; otherwise App runs its normal interactive loop.
    //
    // warmupFrames exists because TAA, ReSTIR and the volumetric resolve all accumulate temporally: a
    // single-frame capture is noise. Jitter and history are keyed per-FrameTargets in ViewResources, so
    // the harness view converges on its own without disturbing any other view.
    struct HarnessSpec
    {
        std::filesystem::path scenePath;
        std::filesystem::path shotPath;
        std::filesystem::path logPath;

        u32 width        = 1280;
        u32 height       = 720;
        u32 warmupFrames = 32;

        bool IsActive() const { return !scenePath.empty() && !shotPath.empty(); }
    };
}
