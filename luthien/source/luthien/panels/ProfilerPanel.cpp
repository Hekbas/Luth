#include "lepch.h"
#include "ProfilerPanel.h"
#include "luth/jobs/JobSystem.h"
#include "luthien/EditorColors.h"
#include "luthien/EditorSnapshot.h"
#include "luth/core/time/Time.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/GPUTimerPool.h"
#include "luth/renderer/rendergraph/RenderGraph.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/resources/AssetManager.h"
#include "luthien/widgets/Icons.h"
#include "luthien/Editor.h"
#include "luthien/widgets/Widgets.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    using Luth::u64;

    std::string FormatCount(u64 n)
    {
        char buf[32];
        if      (n >= 1000000000ull) snprintf(buf, sizeof(buf), "%.2fB", (double)n / 1e9);
        else if (n >= 1000000ull)    snprintf(buf, sizeof(buf), "%.2fM", (double)n / 1e6);
        else if (n >= 1000ull)       snprintf(buf, sizeof(buf), "%.1fK", (double)n / 1e3);
        else                         snprintf(buf, sizeof(buf), "%llu", (unsigned long long)n);
        return buf;
    }

    // Frame-time color follows the active budget (not a hardcoded 60 fps): <=budget good, <=2x watch, else bad.
    ImVec4 FrameColor(float ms, float budget)
    {
        if (ms <= budget)        return ImVec4(0.30f, 0.80f, 0.35f, 1.0f);
        if (ms <= budget * 2.0f) return ImVec4(0.90f, 0.80f, 0.25f, 1.0f);
        return ImVec4(0.90f, 0.32f, 0.32f, 1.0f);
    }
    ImU32 FrameColorU32(float ms, float budget) { return ImGui::GetColorU32(FrameColor(ms, budget)); }

    // Shift the cursor right so content of the given width sits flush against the right margin of the row.
    void RightAlign(float width)
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
    }

    // Small filled pill (badge). Advances the cursor by its size.
    void Pill(const char* text, ImU32 bg, ImU32 fg)
    {
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 p  = ImGui::GetCursorScreenPos();
        const float  px = 8.0f, py = 2.0f;
        const ImVec2 sz = ImVec2(ts.x + px * 2.0f, ts.y + py * 2.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, 4.0f);
        dl->AddText(ImVec2(p.x + px, p.y + py), fg, text);
        ImGui::Dummy(sz);
    }

    const ImU32 kClassColors[6] = {
        IM_COL32(0x37, 0x8A, 0xDD, 255), // Texture       blue
        IM_COL32(0x1D, 0x9E, 0x75, 255), // RenderTarget   teal
        IM_COL32(0x63, 0x99, 0x22, 255), // Mesh           green
        IM_COL32(0x7F, 0x77, 0xDD, 255), // Buffer         purple
        IM_COL32(0xEF, 0x9F, 0x27, 255), // AccelStructure amber
        IM_COL32(0x88, 0x87, 0x80, 255), // Other          gray
    };
    const char* kClassNames[6] = { "Textures", "Render targets", "Meshes", "Buffers", "Accel structures", "Other" };
}

namespace Luth
{
    ProfilerPanel::ProfilerPanel()
    {
        m_WindowID = "Profiler";
        m_FrameTimeHistory.resize(100, 0.0f);
        m_MemoryHistory.resize(100, 0.0f);
    }

    void ProfilerPanel::OnInit() {}

    const char* ProfilerPanel::FormatBytes(i64 bytes, char* buf, size_t bufSize)
    {
        double a = (double)(bytes < 0 ? -bytes : bytes);
        const char* s = bytes < 0 ? "-" : "";
        if      (a >= 1024.0 * 1024.0 * 1024.0) snprintf(buf, bufSize, "%s%.2f GB", s, a / (1024.0 * 1024.0 * 1024.0));
        else if (a >= 1024.0 * 1024.0)          snprintf(buf, bufSize, "%s%.2f MB", s, a / (1024.0 * 1024.0));
        else if (a >= 1024.0)                   snprintf(buf, bufSize, "%s%.2f KB", s, a / 1024.0);
        else                                    snprintf(buf, bufSize, "%s%lld B", s, (long long)(bytes < 0 ? -bytes : bytes));
        return buf;
    }

    void ProfilerPanel::OnGather(EditorSnapshotBuilder& builder)
    {
        builder.Add<ProfilerSnapshot>();
    }

    void ProfilerPanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        ImGui::PushFont(Editor::GetIconRegular());
        std::string title = ICON_CHART + std::string("  Profiler");

        if (BeginWindow(title.c_str()))
        {
            // ── Cached stats at 10 Hz ──
            m_UpdateTimer += Time::UnscaledDeltaTime();
            if (m_UpdateTimer >= 0.1f)
            {
                m_UpdateTimer = 0.0f;
                m_FPS = ImGui::GetIO().Framerate;
                m_FrameTime = 1000.0f / m_FPS;

                std::rotate(m_FrameTimeHistory.begin(), m_FrameTimeHistory.begin() + 1, m_FrameTimeHistory.end());
                m_FrameTimeHistory.back() = m_FrameTime;
                m_FrameTimeMin = *std::min_element(m_FrameTimeHistory.begin(), m_FrameTimeHistory.end());
                m_FrameTimeMax = *std::max_element(m_FrameTimeHistory.begin(), m_FrameTimeHistory.end());
                float sum = 0.0f; for (float v : m_FrameTimeHistory) sum += v;
                m_FrameTimeAvg = sum / (float)m_FrameTimeHistory.size();

                std::vector<float> sorted = m_FrameTimeHistory;
                std::sort(sorted.begin(), sorted.end());
                auto pctl = [&](float p) { return sorted[std::min((size_t)(p * (sorted.size() - 1) + 0.5f), sorted.size() - 1)]; };
                m_FrameTimeP95 = pctl(0.95f);
                m_FrameTimeP99 = pctl(0.99f);
                m_OnePercentLowFps = m_FrameTimeP99 > 0.0001f ? 1000.0f / m_FrameTimeP99 : 0.0f;

                m_MemSnapshot = Memory::MemoryTracker::GetSnapshot();
                std::rotate(m_MemoryHistory.begin(), m_MemoryHistory.begin() + 1, m_MemoryHistory.end());
                m_MemoryHistory.back() = (float)m_MemSnapshot.TotalCurrent / (1024.0f * 1024.0f);
                m_GPUStats = VulkanAllocator::GetStats();
                m_GpuHeapStats = Memory::GPUTaggedPageAllocator::Get().GetStats();

                // Scheduler: snapshot stats, then derive per-worker occupancy from the cumulative-nanos delta.
                m_JobStats = JobSystem::GetStats();
                for (u32 i = 0; i < m_JobStats.ThreadCount && i < JobSystem::MAX_WORKER_THREADS; ++i)
                {
                    u64 total = 0, d[4];
                    for (int s = 0; s < 4; ++s) { d[s] = m_JobStats.PerThreadStateNanos[i][s] - m_PrevStateNanos[i][s]; total += d[s]; }
                    if (m_HavePrevNanos && total > 0)
                        for (int s = 0; s < 4; ++s) m_Occupancy[i][s] = (float)d[s] / (float)total;
                    for (int s = 0; s < 4; ++s) m_PrevStateNanos[i][s] = m_JobStats.PerThreadStateNanos[i][s];
                }
                m_HavePrevNanos = true;

                // Queue depths read ~0 at 10 Hz (work drains between samples) — hold a decaying recent peak.
                u32 maxDeq = 0;
                for (u32 i = 0; i < m_JobStats.ThreadCount && i < JobSystem::MAX_WORKER_THREADS; ++i)
                    maxDeq = std::max(maxDeq, m_JobStats.PerThreadQueued[i]);
                m_QueuePeak = std::max((float)m_JobStats.HighQueueSize, m_QueuePeak * 0.90f);
                m_DequePeak = std::max((float)maxDeq, m_DequePeak * 0.90f);

                if (auto rs = SystemRegistry::GetSystem<RenderingSystem>())
                {
                    const auto& snap = rs->GetGraphSnapshot();
                    m_GPUFrameTimeMs = snap.totalGpuTimeMs;
                    m_TriangleCount  = rs->GetTriangleCount();
                    u32 draws = 0;
                    for (const auto& p : snap.passes) if (!p.culled) draws += p.drawCalls;
                    m_DrawCalls = draws;

                    // EMA-smooth per-pass GPU time (keyed by name) so the sort order doesn't jitter per frame.
                    m_PassRows.clear();
                    for (const auto& p : snap.passes)
                    {
                        if (p.gpuTimeMs < 0.0f) continue;
                        float& ema = m_PassEma[p.name];
                        ema = ema * 0.8f + p.gpuTimeMs * 0.2f;
                        float od = -1.0f;
                        if (p.stats.valid && p.primaryOutputIndex >= 0 && (size_t)p.primaryOutputIndex < snap.resources.size())
                        {
                            const u64 px = (u64)snap.resources[p.primaryOutputIndex].width * snap.resources[p.primaryOutputIndex].height;
                            if (px) od = (float)p.stats.fsInvocations / (float)px;
                        }
                        m_PassRows.push_back({ p.name, ema, od });
                    }
                    std::sort(m_PassRows.begin(), m_PassRows.end(),
                              [](const GpuPassRow& a, const GpuPassRow& b) { return a.ms > b.ms; });
                }

                if (m_TrimFeedbackTimer > 0.0f) m_TrimFeedbackTimer -= 0.1f;
            }

            DrawOverview();
            ImGui::Dummy(ImVec2(0, 6));
            const char* kTabs[] = { "CPU", "Memory", "GPU" };
            UI::SegmentedButton("ProfilerTabs", kTabs, IM_ARRAYSIZE(kTabs), &m_Tab, /*fillWidth*/ true);
            ImGui::Spacing();

            if      (m_Tab == 0) DrawCpuTab();
            else if (m_Tab == 1) DrawMemoryTab();
            else                 DrawGpuTab();
        }
        ImGui::End();
        ImGui::PopFont();
    }

    // ── Pinned overview: CPU/GPU bars, frame graph, percentiles, bound badge, render stats ──
    void ProfilerPanel::DrawOverview()
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("FPS Target");
        ImGui::SameLine();
        ImGui::PushItemWidth(50.0f);
        const char* fmt = (m_TargetFPS == 0) ? "None" : "%d";
        if (ImGui::DragInt("##TargetFPS", &m_TargetFPS, 1.0f, 0, 999, fmt))
        {
            m_TargetFPS = std::max(m_TargetFPS, 0);
            m_FrameBudgetMs = m_TargetFPS > 0 ? 1000.0f / (float)m_TargetFPS : 16.67f;
        }
        ImGui::PopItemWidth();

        // CPU/GPU-bound badge — derived from the two frame times.
        const char* bound; ImU32 bbg, bfg;
        if (m_GPUFrameTimeMs > m_FrameTime * 1.15f)      { bound = "GPU-bound"; bbg = IM_COL32(80, 60, 30, 255);  bfg = IM_COL32(240, 190, 110, 255); }
        else if (m_FrameTime > m_GPUFrameTimeMs * 1.15f) { bound = "CPU-bound"; bbg = IM_COL32(30, 55, 80, 255);  bfg = IM_COL32(120, 180, 240, 255); }
        else                                             { bound = "balanced "; bbg = IM_COL32(55, 55, 55, 255);  bfg = IM_COL32(180, 180, 180, 255); }

        char fpsBuf[32]; snprintf(fpsBuf, sizeof(fpsBuf), "%.0f fps", m_FPS);
        const float badgeW = ImGui::CalcTextSize(bound).x + 16.0f;
        const float fpsW   = ImGui::CalcTextSize(fpsBuf).x;
        const float rightW = fpsW + 10.0f + badgeW;
        if (ImGui::GetContentRegionAvail().x > rightW)
            ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - rightW);
        else
            ImGui::SameLine();
        ImGui::TextColored(FrameColor(m_FrameTime, m_FrameBudgetMs), "%s", fpsBuf);
        ImGui::SameLine();
        Pill(bound, bbg, bfg);

        // CPU + GPU bars (full at 1.5x budget; tick marks the budget). Fixed label/value columns so the
        // bar length doesn't jump when the ms readout gains or loses a digit.
        const float scale = m_FrameBudgetMs * 1.5f;
        const float tick  = (m_TargetFPS > 0) ? (1.0f / 1.5f) : -1.0f;
        const float lblW = 38.0f, valW = 64.0f;
        const float barW = std::max(40.0f, ImGui::GetContentRegionAvail().x - lblW - valW);
        char cpuBuf[24], gpuBuf[24];
        snprintf(cpuBuf, sizeof(cpuBuf), "%.1f ms", m_FrameTime);
        snprintf(gpuBuf, sizeof(gpuBuf), "%.1f ms", m_GPUFrameTimeMs);
        UI::StatBar("CPU", m_FrameTime / scale,      FrameColorU32(m_FrameTime, m_FrameBudgetMs),     cpuBuf, lblW, tick, barW, valW);
        UI::StatBar("GPU", m_GPUFrameTimeMs / scale, FrameColorU32(m_GPUFrameTimeMs, m_FrameBudgetMs), gpuBuf, lblW, tick, barW, valW);

        ImGui::PlotLines("##FrameTimes", m_FrameTimeHistory.data(), (int)m_FrameTimeHistory.size(),
            0, nullptr, 0.0f, m_FrameBudgetMs * 2.0f, ImVec2(ImGui::GetContentRegionAvail().x, 38));

        ImGui::TextColored(FrameColor(m_FrameTimeMin, m_FrameBudgetMs), "min %.1f", m_FrameTimeMin);
        ImGui::SameLine(); ImGui::TextColored(FrameColor(m_FrameTimeAvg, m_FrameBudgetMs), "  avg %.1f", m_FrameTimeAvg);
        ImGui::SameLine(); ImGui::TextColored(FrameColor(m_FrameTimeMax, m_FrameBudgetMs), "  max %.1f", m_FrameTimeMax);
        ImGui::SameLine(); ImGui::TextDisabled("ms");
        ImGui::SameLine(); ImGui::TextColored(FrameColor(m_FrameTimeP99, m_FrameBudgetMs), "   1%% low %.0f fps", m_OnePercentLowFps);
        ImGui::SameLine(); ImGui::TextDisabled(" (p95 %.1f  p99 %.1f)", m_FrameTimeP95, m_FrameTimeP99);

        ImGui::Text("%s tris  %s  %u draws", FormatCount(m_TriangleCount).c_str(),
                    ICON_SHAPES, m_DrawCalls);
        ImGui::SameLine();
#ifdef TRACY_ENABLE
        const char* tracy = "Tracy: on";
#else
        const char* tracy = "Tracy: off";
#endif
        const float tw = ImGui::CalcTextSize(tracy).x + 4.0f;
        if (ImGui::GetContentRegionAvail().x > tw) ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - tw);
        ImGui::TextDisabled("%s", tracy);
    }

    // ── CPU tab: scheduler dashboard ──
    void ProfilerPanel::DrawCpuTab()
    {
        const u32 nThreads = m_JobStats.ThreadCount;

        // Summary metric cards: throughput, occupancy, steal efficiency.
        float occSum = 0.0f; u32 occN = 0;
        for (u32 i = 1; i < nThreads && i < JobSystem::MAX_WORKER_THREADS; ++i) { occSum += m_Occupancy[i][1]; ++occN; }
        const float occAvg = occN ? (occSum / occN) * 100.0f : 0.0f;
        const float stealEff = m_JobStats.StealAttempts ? 100.0f * (float)m_JobStats.StealSuccesses / (float)m_JobStats.StealAttempts : 0.0f;

        char c0[32], c1[24], c2[40];
        snprintf(c0, sizeof(c0), "%s", FormatCount(m_JobStats.JobsExecuted).c_str());
        snprintf(c1, sizeof(c1), "%.0f %%", occAvg);
        snprintf(c2, sizeof(c2), "%.0f%%  %s/%s", stealEff, FormatCount(m_JobStats.StealSuccesses).c_str(), FormatCount(m_JobStats.StealAttempts).c_str());

        const float gap = 10.0f;
        const float cardW = std::floor((ImGui::GetContentRegionAvail().x - gap * 2.0f - 2.0f) / 3.0f);
        UI::MetricCard("jobs / frame", c0, cardW);     ImGui::SameLine(0, gap);
        UI::MetricCard("occupancy", c1, cardW);        ImGui::SameLine(0, gap);
        UI::MetricCard("steal efficiency", c2, cardW);
        ImGui::Spacing();

        // ── Worker occupancy (workers 1..N; the main thread is V2-isolated, not a stealing worker) ──
        UI::SectionHeader("Worker occupancy");
        {
            const char* leg[3]  = { "running", "stealing", "idle" };
            const ImU32  lcol[3] = { EditorColors::WorkerRunning, EditorColors::WorkerStealing, EditorColors::WorkerIdle };
            const float sw = ImGui::GetTextLineHeight() * 0.78f + 5.0f;
            float legendW = 24.0f;
            for (int k = 0; k < 3; ++k) legendW += sw + ImGui::CalcTextSize(leg[k]).x;
            ImGui::SameLine();
            RightAlign(legendW);
            for (int k = 0; k < 3; ++k) { UI::LegendItem(leg[k], lcol[k]); if (k < 2) ImGui::SameLine(0, 12); }
        }

        // Two columns of workers; per-worker jobs/steals + exact occupancy show on hover.
        if (ImGui::BeginTable("##workers", 2, ImGuiTableFlags_SizingStretchSame))
        {
            for (u32 i = 1; i < nThreads && i < JobSystem::MAX_WORKER_THREADS; ++i)
            {
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                char lbl[12]; snprintf(lbl, sizeof(lbl), "W%u", i);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(lbl);
                ImGui::SameLine(30.0f);
                const float run = m_Occupancy[i][1], steal = m_Occupancy[i][2], idle = m_Occupancy[i][0] + m_Occupancy[i][3];
                UI::BarSegment segs[3] = {
                    { run,   EditorColors::WorkerRunning },
                    { steal, EditorColors::WorkerStealing },
                    { idle,  EditorColors::WorkerIdle },
                };
                UI::StackedBar("occ", segs, 3, 13.0f, -1.0f, ImGui::GetContentRegionAvail().x);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Worker %u\nrunning %.0f%%   stealing %.0f%%   idle %.0f%%\n%u jobs   %u steals",
                        i, run * 100.0f, steal * 100.0f, idle * 100.0f, m_JobStats.PerThreadJobs[i], m_JobStats.PerThreadSteals[i]);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // ── Fiber pool — gauge scaled to peak (the 512-slot pool sits mostly idle) + numbers ──
        UI::SectionHeader("Fiber pool");
        {
            const u32 total = m_JobStats.TotalFibers ? m_JobStats.TotalFibers : 1;
            const u32 inUse = total > m_JobStats.FreeFibers ? total - m_JobStats.FreeFibers : 0;
            char info[96]; snprintf(info, sizeof(info), "%u in use   %u ready   peak %u / %u",
                inUse, m_JobStats.ReadyFiberCount, m_JobStats.PeakFibers, total);
            ImGui::SameLine(); RightAlign(ImGui::CalcTextSize(info).x);
            ImGui::TextDisabled("%s", info);

            const float gMax = std::max(32.0f, (float)m_JobStats.PeakFibers * 1.5f);
            UI::BarSegment fib[2] = {
                { (float)inUse / gMax,                      IM_COL32(0x37, 0x8A, 0xDD, 255) },
                { (float)m_JobStats.ReadyFiberCount / gMax, IM_COL32(0x1D, 0x9E, 0x75, 255) },
            };
            UI::StackedBar("fibers", fib, 2, 14.0f, (float)m_JobStats.PeakFibers / gMax);
        }

        // ── Queues — depths drain fast, so show the decaying recent peak alongside the live value ──
        UI::SectionHeader("Queues");
        {
            char info[96]; snprintf(info, sizeof(info), "global %u (peak %.0f)    deque peak %.0f",
                m_JobStats.HighQueueSize, m_QueuePeak, m_DequePeak);
            ImGui::SameLine(); RightAlign(ImGui::CalcTextSize(info).x);
            ImGui::TextDisabled("%s", info);
        }

        // ── Stage split ──
        UI::SectionHeader("Stage split");
        const float sScale = std::max({ m_JobStats.GameStageMs, m_JobStats.RenderStageMs, m_FrameBudgetMs });
        char gb[20], rb[20];
        snprintf(gb, sizeof(gb), "%.1f ms", m_JobStats.GameStageMs);
        snprintf(rb, sizeof(rb), "%.1f ms", m_JobStats.RenderStageMs);
        const float sLblW = 52.0f, sValW = 58.0f;
        const float sBarW = std::max(40.0f, ImGui::GetContentRegionAvail().x - sLblW - sValW);
        UI::StatBar("Game",   m_JobStats.GameStageMs / sScale,   IM_COL32(0xBA, 0x75, 0x17, 255), gb, sLblW, -1.0f, sBarW, sValW);
        UI::StatBar("Render", m_JobStats.RenderStageMs / sScale, IM_COL32(0x53, 0x4A, 0xB7, 255), rb, sLblW, -1.0f, sBarW, sValW);
    }

    // ── Memory tab: system (CPU) + GPU-by-type ──
    void ProfilerPanel::DrawMemoryTab()
    {
        char b1[64], b2[64];

        const int gpuCat       = (int)Memory::Category::GPU;
        const i64 cpuTotal     = std::max<i64>(0, m_MemSnapshot.TotalCurrent - m_MemSnapshot.Categories[gpuCat].Current);
        const ImU32 catColors[] = {
            EditorColors::MemGeneral, EditorColors::MemRendering, EditorColors::MemScene, EditorColors::MemJobs,
            EditorColors::MemResources, EditorColors::MemEditor, EditorColors::MemFrameLinear,
            EditorColors::MemFrameTagged, EditorColors::MemGPU,
        };

        // ── System memory (CPU) — excludes the GPU category (it has its own section below) ──
        UI::SectionHeader("System memory (CPU)");
        {
            char info[96]; snprintf(info, sizeof(info), "%s    peak %s",
                FormatBytes(cpuTotal, b1, sizeof(b1)), FormatBytes(m_MemSnapshot.TotalPeak, b2, sizeof(b2)));
            ImGui::SameLine(); RightAlign(ImGui::CalcTextSize(info).x);
            ImGui::TextDisabled("%s", info);
        }

        const i64 cpuBase = cpuTotal > 0 ? cpuTotal : 1;
        UI::BarSegment catSegs[9]; int catN = 0;
        for (u8 i = 0; i < (u8)Memory::Category::Count && catN < 9; ++i)
        {
            if ((int)i == gpuCat) continue;
            const i64 cur = m_MemSnapshot.Categories[i].Current;
            if (cur <= 0) continue;
            catSegs[catN++] = { (float)cur / (float)cpuBase, catColors[i] };
        }
        UI::StackedBar("catbar", catSegs, catN, 16.0f);

        if (ImGui::BeginTable("##catrows", 2, ImGuiTableFlags_SizingStretchProp))
        {
            for (u8 i = 0; i < (u8)Memory::Category::Count; ++i)
            {
                if ((int)i == gpuCat) continue;
                const i64 cur = m_MemSnapshot.Categories[i].Current;
                if (cur <= 0) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                UI::LegendItem(Memory::MemoryTracker::GetCategoryName((Memory::Category)i), catColors[i]);
                ImGui::TableSetColumnIndex(1);
                const char* sz = FormatBytes(cur, b1, sizeof(b1));
                RightAlign(ImGui::CalcTextSize(sz).x);
                ImGui::TextUnformatted(sz);
            }
            ImGui::EndTable();
        }

        float maxMem = 0.0f; for (float m : m_MemoryHistory) maxMem = std::max(m, maxMem);
        const float dynMax = std::max(64.0f, std::exp2(std::ceil(std::log2(maxMem * 1.2f + 1.0f))));
        UI::AreaGraph("mem", m_MemoryHistory.data(), (int)m_MemoryHistory.size(), dynMax, IM_COL32(0x37, 0x8A, 0xDD, 255), 50.0f);

        ImGui::Spacing();
        if (ImGui::Button("Trim unused")) { m_LastTrimCount = AssetManager::Trim(false); m_TrimFeedbackTimer = 3.0f; }
        ImGui::SameLine();
        if (ImGui::Button("Force trim"))  { m_LastTrimCount = AssetManager::Trim(true);  m_TrimFeedbackTimer = 3.0f; }
        if (m_TrimFeedbackTimer > 0.0f) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.35f, 1.0f), "evicted %u", m_LastTrimCount); }

        // ── GPU memory (by resource type) ──
        UI::SectionHeader("GPU memory");
        {
            const float resvMB = (float)(m_GPUStats.UsedBytes + m_GPUStats.FreeBytes) / (1024.0f * 1024.0f);
            char info[96]; snprintf(info, sizeof(info), "%s used   %.0f MB reserved",
                FormatBytes((i64)m_GPUStats.UsedBytes, b1, sizeof(b1)), resvMB);
            ImGui::SameLine(); RightAlign(ImGui::CalcTextSize(info).x);
            ImGui::TextDisabled("%s", info);
        }

        const u64 classTotal = std::max<u64>(1, m_GPUStats.UsedBytes);
        UI::BarSegment clsSegs[6]; int clsN = 0;
        for (int i = 0; i < 6; ++i)
            if (m_GPUStats.ClassBytes[i] > 0) clsSegs[clsN++] = { (float)((double)m_GPUStats.ClassBytes[i] / (double)classTotal), kClassColors[i] };
        UI::StackedBar("gpucls", clsSegs, clsN, 16.0f);

        if (ImGui::BeginTable("##gpurows", 2, ImGuiTableFlags_SizingStretchProp))
        {
            for (int i = 0; i < 6; ++i)
            {
                if (m_GPUStats.ClassBytes[i] == 0) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                UI::LegendItem(kClassNames[i], kClassColors[i]);
                ImGui::TableSetColumnIndex(1);
                const char* sz = FormatBytes((i64)m_GPUStats.ClassBytes[i], b1, sizeof(b1));
                RightAlign(ImGui::CalcTextSize(sz).x);
                ImGui::TextUnformatted(sz);
            }
            ImGui::EndTable();
        }

        const float inFlightMB = (float)m_GpuHeapStats.BytesInFlight / (1024.0f * 1024.0f);
        ImGui::TextDisabled("VMA: %u allocs  ·  %u blocks  ·  heap %.0f MB in flight",
                            m_GPUStats.AllocationCount, m_GPUStats.BlockCount, inFlightMB);
    }

    // ── GPU tab: metric cards + consolidated per-pass view + barriers + slang parity ──
    void ProfilerPanel::DrawGpuTab()
    {
        auto rs = SystemRegistry::GetSystem<RenderingSystem>();

        char m0[20], m1[20];
        snprintf(m0, sizeof(m0), "%.1f ms", m_GPUFrameTimeMs);
        snprintf(m1, sizeof(m1), "%s", FormatCount(m_TriangleCount).c_str());
        char m2[16]; snprintf(m2, sizeof(m2), "%u", m_DrawCalls);
        int passCount = rs ? (int)rs->GetGraphSnapshot().passes.size() : 0;
        char m3[16]; snprintf(m3, sizeof(m3), "%d", passCount);
        const float gap = 10.0f, cardW = std::floor((ImGui::GetContentRegionAvail().x - gap * 3.0f - 2.0f) / 4.0f);
        UI::MetricCard("GPU time", m0, cardW);  ImGui::SameLine(0, gap);
        UI::MetricCard("triangles", m1, cardW); ImGui::SameLine(0, gap);
        UI::MetricCard("draw calls", m2, cardW);ImGui::SameLine(0, gap);
        UI::MetricCard("passes", m3, cardW);
        ImGui::Spacing();

        // Consolidated per-pass view: GPU-time bar + overdraw chip (when pipeline-stats capture is on).
        const bool statsSupported = VulkanContext::Get().SupportsPipelineStats();
        bool capture = GPUTimerPool::StatsEnabled();
        UI::SectionHeader("Passes by GPU time");
        if (statsSupported && ImGui::Checkbox("capture overdraw", &capture))
            GPUTimerPool::SetStatsEnabled(capture);

        // Table over the EMA-smoothed, pre-sorted rows (m_PassRows) — fixed columns keep the value + overdraw
        // aligned, and smoothing keeps the order from jittering frame-to-frame.
        {
            const float maxMs = m_PassRows.empty() ? 1.0f : std::max(0.001f, m_PassRows.front().ms);
            const int   shown = std::min<int>((int)m_PassRows.size(), 14);
            const int   cols  = capture ? 4 : 3;
            if (ImGui::BeginTable("##passes", cols, ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("pass", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("bar",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ms",   ImGuiTableColumnFlags_WidthFixed, 96.0f);
                if (capture) ImGui::TableSetupColumn("od", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                for (int i = 0; i < shown; ++i)
                {
                    const GpuPassRow& r = m_PassRows[i];
                    const float frac = r.ms / maxMs;
                    const ImU32 col = frac > 0.66f ? IM_COL32(0xE2, 0x4B, 0x4A, 255)
                                    : frac > 0.33f ? IM_COL32(0xEF, 0x9F, 0x27, 255)
                                                   : IM_COL32(0x37, 0x8A, 0xDD, 255);
                    ImGui::TableNextRow();
                    ImGui::PushID(i);
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(r.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    UI::BarSegment seg = { frac, col };
                    UI::StackedBar("b", &seg, 1, 13.0f, -1.0f, ImGui::GetContentRegionAvail().x);
                    ImGui::TableSetColumnIndex(2);
                    char val[32];
                    const float pct = m_GPUFrameTimeMs > 0.0f ? 100.0f * r.ms / m_GPUFrameTimeMs : 0.0f;
                    snprintf(val, sizeof(val), "%.2f ms  %.0f%%", r.ms, pct);
                    RightAlign(ImGui::CalcTextSize(val).x);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", val);
                    if (capture && r.overdraw >= 0.0f)
                    {
                        ImGui::TableSetColumnIndex(3);
                        const ImVec4 oc = r.overdraw <= 1.5f ? ImVec4(0.30f, 0.80f, 0.35f, 1.0f)
                                        : r.overdraw <= 3.0f ? ImVec4(0.90f, 0.80f, 0.25f, 1.0f)
                                                             : ImVec4(0.90f, 0.32f, 0.32f, 1.0f);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(oc, "%.1fx", r.overdraw);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("top passes  ·  full scrub tree in Frame Debugger");
        }

        ImGui::Spacing();
        UI::SectionHeader("Barriers");
        bool bcap = RG::RenderGraph::BarrierCapture();
        if (ImGui::Checkbox("capture", &bcap)) RG::RenderGraph::SetBarrierCapture(bcap);
        if (bcap && rs)
        {
            const auto& snap = rs->GetGraphSnapshot();
            ImGui::SameLine(); ImGui::Checkbox("redundant only", &m_BarrierRedundantOnly);
            ImGui::Text("image %u   buffer %u   redundant %u", snap.numImageBarriers, snap.numBufferBarriers, snap.numRedundantBarriers);
            if (ImGui::BeginTable("##barriers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY, ImVec2(0, 200)))
            {
                ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Transition", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthFixed, 56);
                ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (const auto& b : snap.barriers)
                {
                    if (m_BarrierRedundantOnly && !b.redundant) continue;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(b.resource.c_str());
                    ImGui::TableSetColumnIndex(1);
                    char t[96]; snprintf(t, sizeof(t), "%s -> %s", b.before.c_str(), b.after.c_str());
                    if (b.redundant) ImGui::TextDisabled("%s", t); else ImGui::TextUnformatted(t);
                    ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(b.reason.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(b.passIndex < snap.passes.size() ? snap.passes[b.passIndex].name.c_str() : "?");
                }
                ImGui::EndTable();
            }
        }

        ImGui::Spacing();
        UI::SectionHeader("Slang parity");
        if (rs)
        {
            const auto& sp = rs->GetSlangParitySettings();
            if (!sp.spirvChecked) ImGui::TextDisabled("SPIR-V guard : not run");
            else
            {
                const ImVec4 col = sp.spirvPass ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(0.90f, 0.40f, 0.40f, 1.0f);
                ImGui::TextColored(col, "%s", sp.spirvPass ? "SPIR-V guard pass" : "SPIR-V guard FAIL");
                ImGui::SameLine(); ImGui::TextDisabled("  NonUniform %u   caps %s", sp.nonUniformCount, sp.capsOk ? "present" : "MISSING");
            }
        }
    }
}
