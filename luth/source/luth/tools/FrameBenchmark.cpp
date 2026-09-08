#include "luthpch.h"
#include "luth/tools/FrameBenchmark.h"

#include "luth/core/BuildConfig.h"
#include "luth/core/Version.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"
#include "luth/renderer/backend/vulkan/PipelineCache.h"
#include "luth/renderer/backend/vulkan/VulkanSwapchain.h"
#include "luth/renderer/rendergraph/RenderGraphSnapshot.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/SystemRegistry.h"

#include <algorithm>

namespace Luth
{
    namespace
    {
        struct Dist
        {
            f32 mean = 0.0f, min = 0.0f, max = 0.0f, p95 = 0.0f, p99 = 0.0f;
        };

        // Percentiles by nearest-rank over a sorted copy, matching the editor Profiler's convention so
        // a headless run and the live panel can be compared without reconciling two definitions.
        Dist Summarize(std::vector<f32> v)
        {
            Dist d;
            if (v.empty()) return d;
            std::sort(v.begin(), v.end());
            f64 sum = 0.0;
            for (f32 x : v) sum += x;
            d.mean = (f32)(sum / (f64)v.size());
            d.min  = v.front();
            d.max  = v.back();
            auto pctl = [&](f32 p) {
                const size_t i = (size_t)(p * (f32)(v.size() - 1) + 0.5f);
                return v[std::min(i, v.size() - 1)];
            };
            d.p95 = pctl(0.95f);
            d.p99 = pctl(0.99f);
            return d;
        }

        nlohmann::json ToJson(const Dist& d)
        {
            return { { "mean", d.mean }, { "min", d.min }, { "max", d.max },
                     { "p95", d.p95 },   { "p99", d.p99 } };
        }

        const char* BuildName()
        {
        #if defined(LUTH_BUILD_DEBUG)
            return "Debug";
        #elif defined(LUTH_BUILD_RELEASE)
            return "Release";
        #else
            return "Dist";
        #endif
        }

        const char* PresentName(PresentMode m)
        {
            switch (m) {
                case PresentMode::Immediate: return "immediate";
                case PresentMode::Mailbox:   return "mailbox";
                case PresentMode::Fifo:      return "fifo";
                default:                     return "default";
            }
        }
    }

    bool FrameBenchmark::Sample(bool ready)
    {
        if (m_Done) return true;

        // Pipelines compile lazily on first use, and a compiling frame costs multiples of a steady one.
        // Treat it exactly like unreadiness: a cold cache would otherwise quietly inflate an entire run,
        // which is not a hypothetical -- it is how the first measurements taken with this tool read 4x high.
        const u64 pso = PipelineCache::TotalCompiles();
        const bool compiled = pso != m_LastPsoTotal;
        m_LastPsoTotal = pso;

        // Warmup restarts whenever readiness drops: a scene that streams in a second batch would
        // otherwise open the window mid-load and fold asset upload into the measurement.
        if (!ready || compiled) {
            if (compiled && m_Timing) ++m_PsoRestarts;
            m_Warmup = 0;
            m_Timing = false;
            return false;
        }
        if (++m_Warmup <= m_Spec.warmupFrames) return false;

        // The first post-warmup call only seeds the clock; there is no interval to report yet.
        if (!m_Timing) {
            m_Timing = true;
            m_Clock.Reset();
            return false;
        }

        FrameRow row;
        row.wallMs = m_Clock.ElapsedMillis();
        m_Clock.Reset();

        const JobSystem::Stats js = JobSystem::GetStats();
        row.gameStageMs   = js.GameStageMs;
        row.renderStageMs = js.RenderStageMs;
        row.jobsExecuted  = js.JobsExecuted;
        row.acquireMs     = (f32)VulkanSwapchain::GetLastAcquireMs();
        row.presentMs     = (f32)VulkanSwapchain::GetLastPresentMs();

        if (auto* rs = SystemRegistry::GetSystem<RenderingSystem>())
        {
            const RG::RenderGraphSnapshot& snap = rs->GetGraphSnapshot();
            row.gpuSumMs  = snap.totalGpuTimeMs;
            row.gpuSpanMs = Math::Max(snap.gpuSpanMs, 0.0f);
            for (const auto& p : snap.passes) {
                if (p.culled) continue;
                ++row.passCount;
                row.drawCalls += p.drawCalls;
            }
            AccumulatePasses(snap);
        }

        m_Rows.push_back(row);
        if ((u32)m_Rows.size() >= m_Spec.benchFrames) m_Done = true;
        return m_Done;
    }

    void FrameBenchmark::AccumulatePasses(const RG::RenderGraphSnapshot& snap)
    {
        u32 slot = 0;
        for (const auto& p : snap.passes)
        {
            if (p.culled) continue;
            // Negative is the "no data yet" sentinel from GPUTimerPool's 2-frame readback latency.
            if (p.gpuTimeMs < 0.0f) { ++slot; continue; }

            if (slot >= m_Passes.size()) m_Passes.resize(slot + 1);
            PassAccum& acc = m_Passes[slot];
            if (acc.name.empty()) acc.name = p.name;
            else if (acc.name != p.name) { acc = PassAccum{}; acc.name = p.name; }

            acc.totalMs += p.gpuTimeMs;
            acc.maxMs    = std::max(acc.maxMs, p.gpuTimeMs);
            ++acc.samples;
            ++slot;
        }
    }

    bool FrameBenchmark::Write()
    {
        if (m_Rows.empty()) {
            LH_CORE_ERROR("FrameBenchmark: no frames sampled; nothing to write");
            return false;
        }

        auto column = [&](f32 FrameRow::* field) {
            std::vector<f32> v;
            v.reserve(m_Rows.size());
            for (const FrameRow& r : m_Rows) v.push_back(r.*field);
            return Summarize(std::move(v));
        };

        const Dist wall    = column(&FrameRow::wallMs);
        const Dist game    = column(&FrameRow::gameStageMs);
        const Dist render  = column(&FrameRow::renderStageMs);
        const Dist acquire = column(&FrameRow::acquireMs);
        const Dist present = column(&FrameRow::presentMs);
        const Dist gpuSum  = column(&FrameRow::gpuSumMs);
        const Dist gpuSpan = column(&FrameRow::gpuSpanMs);

        // Same 1.15x rule as the editor Profiler's badge, but against elapsed GPU time rather than the
        // sum of pass times: the sum is work, and only elapsed competes with the CPU for the frame.
        const char* verdict = "balanced";
        if      (gpuSpan.mean > wall.mean * 1.15f) verdict = "gpu-bound";
        else if (wall.mean > gpuSpan.mean * 1.15f) verdict = "cpu-bound";

        nlohmann::json j;
        j["config"] = {
            { "engine",       Luth::GetVersionString() },
            { "build",        BuildName() },
            { "validation",   LUTH_ENABLE_VALIDATION != 0 },
            { "scene",        m_Spec.scenePath.string() },
            { "width",        m_Spec.width },
            { "height",       m_Spec.height },
            { "warmupFrames", m_Spec.warmupFrames },
            { "benchFrames",  m_Spec.benchFrames },
            { "presentMode",  PresentName(m_Spec.presentMode) }
        };

        j["frames"] = {
            { "sampled",       (u32)m_Rows.size() },
            { "fpsFromMean",   wall.mean > 0.0f ? 1000.0f / wall.mean : 0.0f },
            { "wallMs",        ToJson(wall) },
            { "gameStageMs",   ToJson(game) },
            { "renderStageMs", ToJson(render) },
            { "acquireMs",     ToJson(acquire) },
            { "presentMs",     ToJson(present) },
            { "gpuSumMs",      ToJson(gpuSum) },
            { "gpuSpanMs",     ToJson(gpuSpan) },
            { "psoRestarts",   m_PsoRestarts }
        };
        j["verdict"] = verdict;

        // Caveats a reader of this file has to know to not misread it.
        j["notes"] = nlohmann::json::array({
            "gpuSpanMs is elapsed GPU time (earliest begin to latest end) and is what the verdict "
            "compares against wallMs. gpuSumMs is the SUM of per-pass timings: async compute overlaps "
            "graphics so it overstates elapsed time, while barrier bubbles make it understate. Read "
            "gpuSumMs as total GPU work and the gap between the two as overlap minus bubbles.",
            "Per-pass GPU timings lag the CPU row by 2 frames (GPUTimerPool reads frame N-2), so the "
            "aggregates are sound but a single row does not pair its CPU and GPU columns.",
            "psoRestarts > 0 means a pipeline compiled inside the measurement window and warmup was "
            "restarted. A settled run reports 0."
        });

        std::vector<const PassAccum*> ranked;
        ranked.reserve(m_Passes.size());
        for (const PassAccum& p : m_Passes)
            if (p.samples > 0) ranked.push_back(&p);
        std::sort(ranked.begin(), ranked.end(),
                  [](const PassAccum* a, const PassAccum* b) { return a->totalMs > b->totalMs; });

        f64 sumOfMeans = 0.0;
        for (const PassAccum* p : ranked) sumOfMeans += p->totalMs / (f64)p->samples;

        nlohmann::json passes = nlohmann::json::array();
        for (const PassAccum* p : ranked)
        {
            const f64 mean = p->totalMs / (f64)p->samples;
            passes.push_back({
                { "name",    p->name },
                { "meanMs",  mean },
                { "maxMs",   p->maxMs },
                { "samples", p->samples },
                { "pctOfGpu", sumOfMeans > 0.0 ? (mean / sumOfMeans) * 100.0 : 0.0 }
            });
        }
        j["passes"] = std::move(passes);

        nlohmann::json rows = nlohmann::json::array();
        for (const FrameRow& r : m_Rows)
        {
            rows.push_back({
                { "wallMs", r.wallMs }, { "gameStageMs", r.gameStageMs },
                { "renderStageMs", r.renderStageMs }, { "acquireMs", r.acquireMs },
                { "presentMs", r.presentMs }, { "gpuSumMs", r.gpuSumMs }, { "gpuSpanMs", r.gpuSpanMs },
                { "passes", r.passCount }, { "drawCalls", r.drawCalls }, { "jobs", r.jobsExecuted }
            });
        }
        j["rows"] = std::move(rows);

        const auto parent = m_Spec.benchPath.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }

        std::ofstream out(m_Spec.benchPath);
        if (!out) {
            LH_CORE_ERROR("FrameBenchmark: cannot open '{}'", m_Spec.benchPath.string());
            return false;
        }
        out << j.dump(2);

        LH_CORE_INFO("FrameBenchmark: {} frames, {:.3f} ms mean ({:.0f} fps), gpu {:.3f} ms elapsed / "
                     "{:.3f} ms work, {} -- wrote '{}'",
                     m_Rows.size(), wall.mean, wall.mean > 0.0f ? 1000.0f / wall.mean : 0.0f,
                     gpuSpan.mean, gpuSum.mean, verdict, m_Spec.benchPath.string());
        return out.good();
    }
}
