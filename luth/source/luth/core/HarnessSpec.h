#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/platform/Window.h"

#include <filesystem>
#include <string>
#include <vector>

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

        // Quality scoring. hdrPath dumps the LINEAR scene colour, which is the only image worth
        // measuring: the tonemap clips a firefly to white in both the test and the reference, so a
        // comparison of display-encoded PNGs reads near-zero error exactly where the noise is. The PNG
        // from --shot stays the human-viewable output. see tools/ImageMetrics.h
        std::filesystem::path hdrPath;
        std::filesystem::path comparePath;    // reference .hdr to score against
        std::filesystem::path metricsPath;    // scores as JSON
        std::filesystem::path errorMapPath;   // false-colour per-pixel relative error, PNG

        // Hold the capture until the path tracer has accumulated this many samples. 0 = capture on the
        // --frames schedule instead. Sturdier than counting frames because it reads the accumulator
        // rather than assuming samplesPerFrame, and a reset mid-run cannot be mistaken for progress.
        u32 spp = 0;

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

        // Determinism controls. Both default to the reproducible setting whenever the harness is active,
        // because a capture exists to be compared against another capture.
        //
        // Wind is procedural vertex deformation phased on time (deform.slang), enabled by default at
        // strength 0.5, so leaving it on makes two runs bend the same mesh differently and smears a
        // progressive reference bake across the accumulation. --wind on opts back in.
        //
        // timeStep pins Time's clock (see Time::SetFixedDelta): 0 freezes time so wind, gusts and film
        // grain land identically, while TAA jitter / ReSTIR / SVGF keep converging because they index
        // on frame count, not seconds. A positive value advances time reproducibly instead, for
        // measuring temporal stability under motion. Negative restores the live wall clock.
        bool wind     = false;
        f32  timeStep = 0.0f;

        // Volumetric fog stays off unless asked for: the stock density swallows an auto-framed scene.
        // --fog on defers to the saved ViewRenderSettings instead of forcing it either way.
        bool fog = false;

        // Render-settings overrides (--set <section>.<field>=<value>), applied in order after
        // <project>/.luth/render.json loads. An unknown key is fatal, so a typo in a sweep script
        // cannot quietly measure the default and report it as a result.
        std::vector<std::string> overrides;

        bool WantsCapture()   const { return !shotPath.empty() || !hdrPath.empty(); }
        bool WantsMetrics()   const { return !comparePath.empty(); }
        bool WantsBenchmark() const { return !benchPath.empty(); }

        // Gates scene load, offscreen window sizing and the harness objects; both jobs need all three.
        bool IsActive() const { return !scenePath.empty() && (WantsCapture() || WantsBenchmark()); }
    };
}
