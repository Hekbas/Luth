#pragma once

#include "luth/core/HarnessSpec.h"
#include "luth/core/time/Timer.h"
#include "luth/core/types/LuthTypes.h"

#include <string>
#include <vector>

namespace Luth::RG { struct RenderGraphSnapshot; }

namespace Luth
{
    // Headless frame-time recorder for LuthShot (--bench). Every number it reports is already
    // published by the engine for the editor's Profiler panel; this samples the same accessors with
    // no editor attached and writes one JSON report at shutdown, so renderer work can be measured
    // and re-measured without a human reading a live panel. see arch/profiling.md
    //
    // invariant: Sample runs on the main thread at the tail of App::Run's iteration, after
    // Renderer::EndFrame and before JobSystem::ResetFrameStats clears the per-frame counters.
    class FrameBenchmark
    {
    public:
        explicit FrameBenchmark(const HarnessSpec& spec) : m_Spec(spec) {}

        // Records one row once warmup is spent. `ready` is false while the scene is still streaming
        // or the camera is unresolved, which holds the window shut so it measures steady state only.
        // Returns true when the frame budget is spent and the app should stop.
        bool Sample(bool ready);

        // Serializes to m_Spec.benchPath. Called after the loop alongside the JSON log flush.
        bool Write();

        u32 SampledFrames() const { return (u32)m_Rows.size(); }

    private:
        void AccumulatePasses(const RG::RenderGraphSnapshot& snap);

        // One sampled frame. Plain floats so the hot path is a push_back and nothing else.
        struct FrameRow
        {
            f32 wallMs        = 0.0f;
            f32 gameStageMs   = 0.0f;
            f32 renderStageMs = 0.0f;
            f32 acquireMs     = 0.0f;
            f32 presentMs     = 0.0f;
            f32 gpuSumMs      = 0.0f;   // sum of per-pass timings (total GPU work)
            f32 gpuSpanMs     = 0.0f;   // earliest begin to latest end (elapsed GPU time)
            u32 passCount     = 0;
            u32 drawCalls     = 0;
            u32 jobsExecuted  = 0;
        };

        // Per-pass running totals. Resolved positionally because pass order is stable frame to frame,
        // with a name check so a graph that changes shape mid-run re-resolves instead of blending
        // two different passes into one row.
        struct PassAccum
        {
            std::string name;
            f64 totalMs = 0.0;
            f32 maxMs   = 0.0f;
            u32 samples = 0;
        };

        HarnessSpec m_Spec;
        std::vector<FrameRow>  m_Rows;
        std::vector<PassAccum> m_Passes;

        Timer m_Clock;               // interval between successive Samples = the true frame period
        u32   m_Warmup     = 0;
        u64   m_LastPsoTotal = 0;    // diffed per frame; a PSO compile restarts warmup
        u32   m_PsoRestarts  = 0;    // reported, so a run that never settled is visible in the output
        bool  m_Timing     = false;  // false until the first post-warmup Sample seeds m_Clock
        bool  m_Done       = false;
    };
}
