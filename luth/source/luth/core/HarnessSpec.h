#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/platform/Window.h"

#include <filesystem>

namespace Luth
{
    // Offscreen tooling request parsed from argv by App::ParseCommandLineArgs. Drives two independent
    // jobs that share a scene, a camera and an offscreen window: PNG capture (--shot) and frame-time
    // benchmarking (--bench). Either alone is valid; both together capture and measure in one run.
    //
    // warmupFrames exists because TAA, ReSTIR and the volumetric resolve all accumulate temporally: a
    // single-frame capture is noise, and a benchmark window opened before convergence measures the
    // ramp rather than steady state. Jitter and history are keyed per-FrameTargets in ViewResources,
    // so the harness view converges on its own without disturbing any other view.
    struct HarnessSpec
    {
        std::filesystem::path scenePath;
        std::filesystem::path shotPath;
        std::filesystem::path logPath;
        std::filesystem::path benchPath;

        u32 width        = 1280;
        u32 height       = 720;
        u32 warmupFrames = 32;
        u32 benchFrames  = 300;

        // Benchmarks want Immediate: Mailbox couples frame pacing to the presentation engine, which
        // shows up as acquire stall rather than as the render work we are trying to measure.
        PresentMode presentMode = PresentMode::Default;

        // Half-resolution overrides for the RT channels (--half). -1 leaves the built-in default alone;
        // 0/1 force it. Exists so an A/B runs against one binary under one set of machine conditions,
        // which is the only way to compare when something else on the box is also using the GPU.
        i8 halfDi = -1, halfGi = -1, halfRefl = -1;

        bool WantsCapture()   const { return !shotPath.empty(); }
        bool WantsBenchmark() const { return !benchPath.empty(); }

        // Gates scene load, offscreen window sizing and the harness objects; both jobs need all three.
        bool IsActive() const { return !scenePath.empty() && (WantsCapture() || WantsBenchmark()); }
    };
}
