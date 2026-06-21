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
#include "luth/scene/systems/LightingSystem.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/resources/AssetManager.h"
#include "luthien/widgets/Icons.h"
#include "luthien/Editor.h"
#include "luthien/widgets/Widgets.h"

#include <imgui.h>
#include <algorithm>
#include <vector>

namespace
{
    // Compact count: 1234567 -> "1.23M", 4567 -> "4.6K".
    std::string FormatCount(Luth::u64 n)
    {
        char buf[32];
        if      (n >= 1000000000ull) snprintf(buf, sizeof(buf), "%.2fB", (double)n / 1e9);
        else if (n >= 1000000ull)    snprintf(buf, sizeof(buf), "%.2fM", (double)n / 1e6);
        else if (n >= 1000ull)       snprintf(buf, sizeof(buf), "%.1fK", (double)n / 1e3);
        else                         snprintf(buf, sizeof(buf), "%llu", (unsigned long long)n);
        return buf;
    }

    void ColoredProgressBar(float ratio, const ImVec2& size, const char* overlay)
    {
        ImVec4 color = (ratio < 0.50f) ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f)
                     : (ratio < 0.75f) ? ImVec4(0.9f, 0.8f, 0.1f, 1.0f)
                                       : ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
        ImGui::ProgressBar(ratio, size, overlay);
        ImGui::PopStyleColor();
    }

    ImVec4 FrameTimeColor(float ms)
    {
        if (ms < 16.0f) return ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        if (ms < 33.0f) return ImVec4(0.9f, 0.8f, 0.1f, 1.0f);
        return ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
    }

    ImU32 WorkerStateColor(Luth::JobSystem::WorkerState s)
    {
        switch (s) {
            case Luth::JobSystem::WorkerState::Running:  return Luth::EditorColors::WorkerRunning;
            case Luth::JobSystem::WorkerState::Stealing: return Luth::EditorColors::WorkerStealing;
            case Luth::JobSystem::WorkerState::Sleeping: return Luth::EditorColors::WorkerSleeping;
            default:                                     return Luth::EditorColors::WorkerIdle;
        }
    }

    const char* WorkerStateName(Luth::JobSystem::WorkerState s)
    {
        switch (s) {
            case Luth::JobSystem::WorkerState::Running:  return "Running";
            case Luth::JobSystem::WorkerState::Stealing: return "Stealing";
            case Luth::JobSystem::WorkerState::Sleeping: return "Sleeping";
            default:                                     return "Idle";
        }
    }

    // Inline color-swatch + label, for the always-visible worker legend.
    void LegendItem(const char* name, ImU32 col)
    {
        const float sz = ImGui::GetTextLineHeight() * 0.85f;
        ImGui::ColorButton(name, ImGui::ColorConvertU32ToFloat4(col),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(sz, sz));
        ImGui::SameLine(0, 4);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name);
    }
}

namespace Luth
{
    ProfilerPanel::ProfilerPanel()
    {
        m_WindowID = "Profiler";
        m_FrameTimeHistory.resize(100, 0.0f);
        m_MemoryHistory.resize(100, 0.0f);
    }

    void ProfilerPanel::OnInit()
    {
    }

    const char* ProfilerPanel::FormatBytes(i64 bytes, char* buf, size_t bufSize)
    {
        double absBytes = (double)(bytes < 0 ? -bytes : bytes);
        const char* sign = bytes < 0 ? "-" : "";

        if (absBytes >= 1024.0 * 1024.0 * 1024.0)
            snprintf(buf, bufSize, "%s%.2f GB", sign, absBytes / (1024.0 * 1024.0 * 1024.0));
        else if (absBytes >= 1024.0 * 1024.0)
            snprintf(buf, bufSize, "%s%.2f MB", sign, absBytes / (1024.0 * 1024.0));
        else if (absBytes >= 1024.0)
            snprintf(buf, bufSize, "%s%.2f KB", sign, absBytes / 1024.0);
        else
            snprintf(buf, bufSize, "%s%lld B", sign, (long long)(bytes < 0 ? -bytes : bytes));
        return buf;
    }

    void ProfilerPanel::OnGather(EditorSnapshotBuilder& builder)
    {
        // Stat aggregation reads globals (FPS ring, MemoryTracker, JobSystem, GPU memory) — a future
        // OnGather move once the gather phase owns these reads. Inline today.
        builder.Add<ProfilerSnapshot>();
    }

    void ProfilerPanel::OnDraw(const EditorSnapshot& /*snapshot*/)
    {
        LH_PROFILE_FUNCTION();
        ImGui::PushFont(Editor::GetFASolid());
        std::string title = ICON_FA_CHART_LINE + std::string("  Profiler");

        if (BeginWindow(title.c_str()))
        {
            // ── Per-frame sampling (worker states + job counters; must run every frame) ──
            {
                JobSystem::Stats stats = JobSystem::GetStats();
                m_WorkerThreadCount    = stats.ThreadCount;
                m_CachedJobsExecuted   = stats.JobsExecuted;
                m_CachedStealSuccesses = stats.StealSuccesses;
                m_GameStageMs   = stats.GameStageMs;
                m_RenderStageMs = stats.RenderStageMs;

                for (u32 i = 0; i < stats.ThreadCount && i < JobSystem::MAX_WORKER_THREADS; ++i)
                    m_WorkerStateHistory[i][m_WorkerHistoryHead] = stats.PerThreadState[i];
                m_WorkerHistoryHead = (m_WorkerHistoryHead + 1) % WORKER_HISTORY_FRAMES;

                if (auto rs = SystemRegistry::GetSystem<RenderingSystem>())
                    m_GPUFrameTimeMs = rs->GetGraphSnapshot().totalGpuTimeMs;
                if (auto ls = SystemRegistry::GetSystem<LightingSystem>())
                    m_PointLightCount = static_cast<u32>(ls->GetLights().points.size());
            }

            // ── Cached stats at 10Hz (FPS, frame-time ring + percentiles, memory, GPU memory/heap) ──
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
                float sum = 0.0f;
                for (float v : m_FrameTimeHistory) sum += v;
                m_FrameTimeAvg = sum / (float)m_FrameTimeHistory.size();

                // Percentiles over the (10Hz-sampled) ring. Unfilled-prefix zeros sort low, so the high
                // percentiles read true once a few seconds of history exist. 1%-low fps = 1000 / p99.
                std::vector<float> sortedFt = m_FrameTimeHistory;
                std::sort(sortedFt.begin(), sortedFt.end());
                auto pctl = [&](float p) -> float {
                    size_t i = (size_t)(p * (sortedFt.size() - 1) + 0.5f);
                    return sortedFt[std::min(i, sortedFt.size() - 1)];
                };
                m_FrameTimeP95 = pctl(0.95f);
                m_FrameTimeP99 = pctl(0.99f);
                m_OnePercentLowFps = m_FrameTimeP99 > 0.0001f ? 1000.0f / m_FrameTimeP99 : 0.0f;

                m_MemSnapshot = Memory::MemoryTracker::GetSnapshot();
                std::rotate(m_MemoryHistory.begin(), m_MemoryHistory.begin() + 1, m_MemoryHistory.end());
                m_MemoryHistory.back() = (float)m_MemSnapshot.TotalCurrent / (1024.0f * 1024.0f);

                m_GPUStats = VulkanAllocator::GetStats();
                m_GpuHeapStats = Memory::GPUTaggedPageAllocator::Get().GetStats();

                if (m_TrimFeedbackTimer > 0.0f)
                    m_TrimFeedbackTimer -= 0.1f;
            }

            DrawOverview();

            ImGui::Separator();
            const char* kTabs[] = { "CPU", "Memory", "GPU" };
            UI::SegmentedButton("ProfilerTabs", kTabs, IM_ARRAYSIZE(kTabs), &m_Tab);
            ImGui::Spacing();

            if      (m_Tab == 0) DrawCpuTab();
            else if (m_Tab == 1) DrawMemoryTab();
            else                 DrawGpuTab();
        }
        ImGui::End();
        ImGui::PopFont();
    }

    // ── Pinned overview: budget bars, frame graph, frame-time stats + percentiles, Tracy state ──
    void ProfilerPanel::DrawOverview()
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("FPS Target");
        ImGui::SameLine();
        ImGui::PushItemWidth(50.0f);
        const char* fmt = (m_TargetFPS == 0) ? "None" : "%d";
        if (ImGui::DragInt("##TargetFPS", &m_TargetFPS, 1.0f, 0, 999, fmt)) {
            m_TargetFPS = std::max(m_TargetFPS, 0);
            m_FrameBudgetMs = m_TargetFPS > 0 ? 1000.0f / (float)m_TargetFPS : 16.67f;
        }
        ImGui::PopItemWidth();

        // Right-aligned live FPS readout.
        char fpsBuf[32];
        snprintf(fpsBuf, sizeof(fpsBuf), "%.0f fps", m_FPS);
        float w = ImGui::CalcTextSize(fpsBuf).x;
        if (ImGui::GetContentRegionAvail().x > w)
            ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
        else
            ImGui::SameLine();
        ImGui::TextColored(FrameTimeColor(m_FrameTime), "%s", fpsBuf);

        // Budget bars (Game/Render run concurrently in steady state, so their sum can exceed CPU).
        const float barH = 20.0f;
        const float availW = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        auto bar = [&](const char* label, float ms, ImU32 color, bool overBudgetCheck) {
            ImVec2 c = ImGui::GetCursorScreenPos();
            float ratio = ms / m_FrameBudgetMs;
            float fillW = std::min(ratio, 1.5f) / 1.5f * availW;
            bool over = overBudgetCheck && m_TargetFPS > 0 && ms > m_FrameBudgetMs;
            ImU32 fill = over ? IM_COL32(200, 60, 60, 255) : color;
            dl->AddRectFilled(c, ImVec2(c.x + availW, c.y + barH), IM_COL32(40, 40, 40, 255), 2.0f);
            dl->AddRectFilled(c, ImVec2(c.x + fillW, c.y + barH), fill, 2.0f);
            if (m_TargetFPS > 0) {
                float bx = c.x + (1.0f / 1.5f) * availW;
                dl->AddLine(ImVec2(bx, c.y), ImVec2(bx, c.y + barH), IM_COL32(255, 255, 255, 180), 1.0f);
            }
            char buf[48];
            snprintf(buf, sizeof(buf), "%s: %.1f ms", label, ms);
            dl->AddText(ImVec2(c.x + 4, c.y + 2), IM_COL32(255, 255, 255, 255), buf);
            ImGui::Dummy(ImVec2(availW, barH + 2));
        };
        bar("CPU",    m_FrameTime,     IM_COL32(80, 180, 80, 255),  true);
        bar("Game",   m_GameStageMs,   IM_COL32(180, 140, 60, 255), false);
        bar("Render", m_RenderStageMs, IM_COL32(140, 80, 180, 255), false);
        bar("GPU",    m_GPUFrameTimeMs > 0.0f ? m_GPUFrameTimeMs : 0.0f, IM_COL32(60, 120, 200, 255), true);

        ImGui::Separator();
        float graphW = ImGui::GetContentRegionAvail().x;
        ImGui::PlotLines("##FrameTimes", m_FrameTimeHistory.data(), (int)m_FrameTimeHistory.size(),
            0, nullptr, 0.0f, m_FrameBudgetMs * 2.0f, ImVec2(graphW, 40));

        ImGui::TextColored(FrameTimeColor(m_FrameTimeMin), "Min %.1f", m_FrameTimeMin);
        ImGui::SameLine(); ImGui::TextColored(FrameTimeColor(m_FrameTimeAvg), "  Avg %.1f", m_FrameTimeAvg);
        ImGui::SameLine(); ImGui::TextColored(FrameTimeColor(m_FrameTimeMax), "  Max %.1f", m_FrameTimeMax);
        ImGui::SameLine(); ImGui::TextDisabled("ms");

        ImGui::TextColored(FrameTimeColor(m_FrameTimeP99), "1%% Low %.0f fps", m_OnePercentLowFps);
        ImGui::SameLine(); ImGui::TextDisabled("  p95 %.1f  p99 %.1f ms", m_FrameTimeP95, m_FrameTimeP99);

        ImGui::Text("Point lights: %u", m_PointLightCount);
        ImGui::SameLine();
#ifdef TRACY_ENABLE
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "   Tracy: enabled");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Attach Tracy.exe to stream CPU zones, GPU timeline, and memory live.");
#else
        ImGui::TextDisabled("   Tracy: compiled out (Dist)");
#endif
    }

    // ── CPU tab: worker timeline swimlanes + always-visible legend ──
    void ProfilerPanel::DrawCpuTab()
    {
        ImGui::TextDisabled("%d workers   %d jobs   %d steals",
            m_WorkerThreadCount, m_CachedJobsExecuted, m_CachedStealSuccesses);

        LegendItem("Running",  EditorColors::WorkerRunning);  ImGui::SameLine(0, 12);
        LegendItem("Stealing", EditorColors::WorkerStealing); ImGui::SameLine(0, 12);
        LegendItem("Idle",     EditorColors::WorkerIdle);     ImGui::SameLine(0, 12);
        LegendItem("Sleeping", EditorColors::WorkerSleeping);
        ImGui::Separator();

        const float availW = ImGui::GetContentRegionAvail().x;
        float segW = availW / (float)WORKER_HISTORY_FRAMES;
        if (segW < 1.0f) segW = 1.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const float mainH = 20.0f, workerH = 8.0f, gap = 1.0f;

        ImGui::Text("Main Thread");
        {
            ImVec2 c = ImGui::GetCursorScreenPos();
            for (u32 f = 0; f < WORKER_HISTORY_FRAMES; ++f) {
                u32 h = (m_WorkerHistoryHead + f) % WORKER_HISTORY_FRAMES;
                float x = c.x + f * segW;
                dl->AddRectFilled(ImVec2(x, c.y), ImVec2(x + segW, c.y + mainH), WorkerStateColor(m_WorkerStateHistory[0][h]));
            }
            ImGui::InvisibleButton("##main_bar", ImVec2(availW, mainH));
            if (ImGui::IsItemHovered()) {
                u32 frame = (u32)((ImGui::GetMousePos().x - ImGui::GetItemRectMin().x) / segW);
                if (frame < WORKER_HISTORY_FRAMES) {
                    u32 h = (m_WorkerHistoryHead + frame) % WORKER_HISTORY_FRAMES;
                    ImGui::SetTooltip("Main Thread: %s", WorkerStateName(m_WorkerStateHistory[0][h]));
                }
            }
        }

        ImGui::Dummy(ImVec2(0, 4));

        if (m_WorkerThreadCount > 1) {
            ImGui::Text("Workers");
            ImVec2 c = ImGui::GetCursorScreenPos();
            float totalH = (m_WorkerThreadCount - 1) * (workerH + gap);
            for (u32 wk = 1; wk < m_WorkerThreadCount && wk < JobSystem::MAX_WORKER_THREADS; ++wk) {
                float yOff = (wk - 1) * (workerH + gap);
                for (u32 f = 0; f < WORKER_HISTORY_FRAMES; ++f) {
                    u32 h = (m_WorkerHistoryHead + f) % WORKER_HISTORY_FRAMES;
                    float x = c.x + f * segW, y = c.y + yOff;
                    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + segW, y + workerH), WorkerStateColor(m_WorkerStateHistory[wk][h]));
                }
            }
            ImGui::InvisibleButton("##worker_bars", ImVec2(availW, totalH));
            if (ImGui::IsItemHovered()) {
                float mx = ImGui::GetMousePos().x - ImGui::GetItemRectMin().x;
                float my = ImGui::GetMousePos().y - ImGui::GetItemRectMin().y;
                u32 frame = (u32)(mx / segW);
                u32 worker = 1 + (u32)(my / (workerH + gap));
                if (frame < WORKER_HISTORY_FRAMES && worker < m_WorkerThreadCount) {
                    u32 h = (m_WorkerHistoryHead + frame) % WORKER_HISTORY_FRAMES;
                    ImGui::SetTooltip("Worker %d: %s", worker, WorkerStateName(m_WorkerStateHistory[worker][h]));
                }
            }
        }
    }

    // ── Memory tab: category stacked bar + Trim, GPU memory, GPU tagged heap ──
    void ProfilerPanel::DrawMemoryTab()
    {
        char buf1[64], buf2[64];
        ImGui::Text("Total: %s (Peak: %s)",
            FormatBytes(m_MemSnapshot.TotalCurrent, buf1, sizeof(buf1)),
            FormatBytes(m_MemSnapshot.TotalPeak, buf2, sizeof(buf2)));

        // History graph — dynamic ceiling snapped to the next power-of-two MB (min 64 MB).
        float maxMem = 0.0f;
        for (float m : m_MemoryHistory) maxMem = std::max(m, maxMem);
        float dynamicMax = std::max(64.0f, std::exp2(std::ceil(std::log2(maxMem * 1.2f))));
        float graphW = ImGui::GetContentRegionAvail().x;
        ImGui::PlotLines("##MemoryUsage", m_MemoryHistory.data(), (int)m_MemoryHistory.size(),
            0, nullptr, 0.0f, dynamicMax, ImVec2(graphW, 40));
        ImGui::Separator();

        const ImU32 categoryColors[] = {
            EditorColors::MemGeneral, EditorColors::MemRendering, EditorColors::MemScene,
            EditorColors::MemJobs, EditorColors::MemResources, EditorColors::MemEditor,
            EditorColors::MemFrameLinear, EditorColors::MemFrameTagged, EditorColors::MemGPU
        };

        const float barH = 20.0f;
        const float availW = ImGui::GetContentRegionAvail().x;
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(cursor, ImVec2(cursor.x + availW, cursor.y + barH), IM_COL32(40, 40, 40, 255), 2.0f);

        i64 totalBytes = m_MemSnapshot.TotalCurrent > 0 ? m_MemSnapshot.TotalCurrent : 1;
        struct Seg { u8 cat; float x; float w; };
        Seg segments[9]{};
        u32 segCount = 0;
        float xOff = 0.0f;
        for (u8 i = 0; i < static_cast<u8>(Memory::Category::Count); ++i) {
            i64 cur = m_MemSnapshot.Categories[i].Current;
            if (cur <= 0) continue;
            float segW = std::max(1.0f, ((float)cur / (float)totalBytes) * availW);
            dl->AddRectFilled(ImVec2(cursor.x + xOff, cursor.y), ImVec2(cursor.x + xOff + segW, cursor.y + barH), categoryColors[i]);
            segments[segCount++] = { i, xOff, segW };
            xOff += segW;
        }

        ImGui::InvisibleButton("##membar", ImVec2(availW, barH));
        if (ImGui::IsItemHovered()) {
            float mx = ImGui::GetMousePos().x - ImGui::GetItemRectMin().x;
            for (u32 s = 0; s < segCount; ++s)
                if (mx >= segments[s].x && mx < segments[s].x + segments[s].w) {
                    i64 cur = m_MemSnapshot.Categories[segments[s].cat].Current;
                    float pct = (float)cur / (float)totalBytes * 100.0f;
                    ImGui::SetTooltip("%s: %s (%.1f%%)",
                        Memory::MemoryTracker::GetCategoryName(static_cast<Memory::Category>(segments[s].cat)),
                        FormatBytes(cur, buf1, sizeof(buf1)), pct);
                    break;
                }
        }

        // Legend (color + name + size).
        if (ImGui::BeginTable("MemoryLegendTable", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            const float lh = ImGui::GetTextLineHeight();
            const float box = lh * 0.8f, vOff = (lh - box) * 0.5f;
            for (u32 s = 0; s < segCount; ++s) {
                ImGui::PushID(s);
                ImGui::TableNextRow();
                u8 cat = segments[s].cat;
                ImGui::TableSetColumnIndex(0);
                float y0 = ImGui::GetCursorPosY();
                ImGui::SetCursorPosY(y0 + vOff);
                ImGui::ColorButton("##mc", ImGui::ColorConvertU32ToFloat4(categoryColors[cat]),
                                   ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(box, box));
                ImGui::SameLine();
                ImGui::SetCursorPosY(y0);
                ImGui::TextUnformatted(Memory::MemoryTracker::GetCategoryName(static_cast<Memory::Category>(cat)));
                ImGui::TableSetColumnIndex(1);
                const char* sz = FormatBytes(m_MemSnapshot.Categories[cat].Current, buf1, sizeof(buf1));
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetColumnWidth() - ImGui::CalcTextSize(sz).x));
                ImGui::TextUnformatted(sz);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (ImGui::Button("Trim Unused Assets")) { m_LastTrimCount = AssetManager::Trim(false); m_TrimFeedbackTimer = 3.0f; }
        ImGui::SameLine();
        if (ImGui::Button("Force Trim"))         { m_LastTrimCount = AssetManager::Trim(true);  m_TrimFeedbackTimer = 3.0f; }
        if (m_TrimFeedbackTimer > 0.0f)
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Evicted %u assets", m_LastTrimCount);

        ImGui::Separator();
        ImGui::SeparatorText("GPU Memory (VMA)");
        {
            float usedMB = (float)m_GPUStats.UsedBytes / (1024.0f * 1024.0f);
            float freeMB = (float)m_GPUStats.FreeBytes / (1024.0f * 1024.0f);
            float totalMB = usedMB + freeMB;
            if (UI::BeginInfoTable("GPUMem")) {
                UI::InfoRow("Used",        "%.2f MB", usedMB);
                UI::InfoRow("Free",        "%.2f MB", freeMB);
                UI::InfoRow("Total",       "%.2f MB", totalMB);
                UI::InfoRow("Allocations", "%u", m_GPUStats.AllocationCount);
                UI::InfoRow("Blocks",      "%u", m_GPUStats.BlockCount);
                UI::EndInfoTable();
            }
            if (totalMB > 0.0f) ColoredProgressBar(usedMB / totalMB, ImVec2(-1, 0), nullptr);
        }

        ImGui::SeparatorText("GPU Tagged Heap (Onion/Garlic)");
        {
            const f32 inFlightMB = (f32)m_GpuHeapStats.BytesInFlight / (1024.0f * 1024.0f);
            if (UI::BeginInfoTable("GpuHeap")) {
                UI::InfoRow("Backing buffers", "%u (x 64 MB)", m_GpuHeapStats.BackingBuffers);
                UI::InfoRow("Active pages",    "%u", m_GpuHeapStats.ActivePages);
                UI::InfoRow("Free pages",      "%u", m_GpuHeapStats.FreePages);
                UI::InfoRow("Large one-shots", "%u", m_GpuHeapStats.LargeOneShots);
                UI::InfoRow("In flight",       "%.2f MB", inFlightMB);
                UI::EndInfoTable();
            }
        }
    }

    // ── GPU tab: per-pass timing hot-list + pipeline stats (overdraw) + barrier inspector ──
    void ProfilerPanel::DrawGpuTab()
    {
        auto rs = SystemRegistry::GetSystem<RenderingSystem>();

        // Top-N GPU-pass hot-list — always-on, sorted by GPU ms. The Frame Debugger owns the full
        // per-pass scrub tree; this is the at-a-glance bottleneck view.
        ImGui::SeparatorText("GPU Passes");
        if (rs) {
            const auto& snap = rs->GetGraphSnapshot();
            struct PT { const char* name; float ms; };
            std::vector<PT> timed;
            timed.reserve(snap.passes.size());
            for (const auto& p : snap.passes)
                if (p.gpuTimeMs >= 0.0f) timed.push_back({ p.name.c_str(), p.gpuTimeMs });
            std::sort(timed.begin(), timed.end(), [](const PT& a, const PT& b) { return a.ms > b.ms; });

            ImGui::Text("Total %.2f ms   |   %zu timed passes", snap.totalGpuTimeMs, timed.size());
            ImGui::TextDisabled("Top passes (full scrub tree lives in Frame Debugger)");

            const float maxMs = timed.empty() ? 1.0f : std::max(0.001f, timed.front().ms);
            if (ImGui::BeginTable("##gpupasses", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
                ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("ms",   ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                const int shown = std::min<int>((int)timed.size(), 15);
                for (int i = 0; i < shown; ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(timed[i].name);
                    ImGui::TableSetColumnIndex(1);
                    char lbl[24]; snprintf(lbl, sizeof(lbl), "%.3f", timed[i].ms);
                    ImGui::ProgressBar(timed[i].ms / maxMs, ImVec2(-1, 0), lbl);
                }
                ImGui::EndTable();
            }
        }

        // Pipeline stats — per-pass overdraw (FS invocations / target pixels). Runtime-toggled.
        ImGui::SeparatorText("Pipeline Stats (overdraw)");
        if (!VulkanContext::Get().SupportsPipelineStats()) {
            ImGui::TextColored(ImVec4(0.80f, 0.60f, 0.20f, 1.0f), "Unsupported on this GPU");
        } else {
            bool enabled = GPUTimerPool::StatsEnabled();
            if (ImGui::Checkbox("Capture (graphics passes)", &enabled))
                GPUTimerPool::SetStatsEnabled(enabled);

            auto sr = enabled ? rs : nullptr;
            if (sr && sr->GetGraphSnapshot().totalStats.valid) {
                const auto& snap = sr->GetGraphSnapshot();
                const auto& t = snap.totalStats;
                const u64 culled = t.inputPrimitives > t.clipPrimitives ? t.inputPrimitives - t.clipPrimitives : 0;
                const float cullP = t.inputPrimitives ? 100.0f * (float)culled / (float)t.inputPrimitives : 0.0f;
                ImGui::Text("Triangles %s    Culled %.0f%%    Shaded %s frag",
                    FormatCount(t.inputPrimitives).c_str(), cullP, FormatCount(t.fsInvocations).c_str());

                if (ImGui::BeginTable("##ppstats", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
                    ImGui::TableSetupColumn("Pass",     ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Tris",     ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("Shaded",   ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("Overdraw", ImGuiTableColumnFlags_WidthFixed, 116);
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();
                    for (const auto& p : snap.passes) {
                        if (!p.stats.valid) continue;
                        u64 px = 0;
                        if (p.primaryOutputIndex >= 0 && (size_t)p.primaryOutputIndex < snap.resources.size())
                            px = (u64)snap.resources[p.primaryOutputIndex].width * snap.resources[p.primaryOutputIndex].height;
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(p.name.c_str());
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(FormatCount(p.stats.inputPrimitives).c_str());
                        ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(FormatCount(p.stats.fsInvocations).c_str());
                        ImGui::TableSetColumnIndex(3);
                        if (px) {
                            const float od = (float)p.stats.fsInvocations / (float)px;
                            const ImVec4 c = od <= 1.5f ? ImVec4(0.30f, 0.80f, 0.35f, 1.0f)
                                           : od <= 3.0f ? ImVec4(0.90f, 0.80f, 0.25f, 1.0f)
                                                        : ImVec4(0.90f, 0.32f, 0.32f, 1.0f);
                            char lbl[16]; snprintf(lbl, sizeof(lbl), "%.2fx", od);
                            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, c);
                            ImGui::ProgressBar(std::min(od / 4.0f, 1.0f), ImVec2(-1, 0), lbl);
                            ImGui::PopStyleColor();
                        } else ImGui::TextDisabled("-");
                    }
                    ImGui::EndTable();
                }
            } else if (enabled) {
                ImGui::TextDisabled("Capturing... (2-frame latency)");
            }
        }

        // Barrier inspector — solved RG barriers (resource, before->after, reason, redundant). Runtime-toggled.
        ImGui::SeparatorText("Barriers");
        bool bcap = RG::RenderGraph::BarrierCapture();
        if (ImGui::Checkbox("Capture barriers", &bcap))
            RG::RenderGraph::SetBarrierCapture(bcap);

        if (bcap && rs) {
            const auto& snap = rs->GetGraphSnapshot();
            ImGui::SameLine();
            ImGui::Checkbox("Redundant only", &m_BarrierRedundantOnly);
            ImGui::Text("img %u   buf %u   redundant %u",
                snap.numImageBarriers, snap.numBufferBarriers, snap.numRedundantBarriers);

            if (ImGui::BeginTable("##barriers", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, 240))) {
                ImGui::TableSetupColumn("Resource",   ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Type",       ImGuiTableColumnFlags_WidthFixed, 38);
                ImGui::TableSetupColumn("Transition", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Reason",     ImGuiTableColumnFlags_WidthFixed, 56);
                ImGui::TableSetupColumn("Pass",       ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (const auto& b : snap.barriers) {
                    if (m_BarrierRedundantOnly && !b.redundant) continue;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(b.resource.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(b.isImage ? "img" : "buf");
                    ImGui::TableSetColumnIndex(2);
                    char trans[96];
                    snprintf(trans, sizeof(trans), "%s -> %s", b.before.c_str(), b.after.c_str());
                    if (b.redundant) ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "%s", trans);
                    else             ImGui::TextUnformatted(trans);
                    ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(b.reason.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(b.passIndex < snap.passes.size() ? snap.passes[b.passIndex].name.c_str() : "?");
                }
                ImGui::EndTable();
            }
        }

        // Slang parity guard — SPIR-V codegen regression check (read-only; a renderer dev diagnostic).
        ImGui::SeparatorText("Slang Parity");
        if (rs) {
            const auto& sp = rs->GetSlangParitySettings();
            if (!sp.spirvChecked) {
                ImGui::TextDisabled("SPIR-V guard : not run");
            } else {
                const ImVec4 col = sp.spirvPass ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(0.90f, 0.40f, 0.40f, 1.0f);
                ImGui::TextColored(col, "SPIR-V guard : %s", sp.spirvPass ? "PASS" : "FAIL");
                ImGui::Text("NonUniform   : %u", sp.nonUniformCount);
                ImGui::Text("bindless caps: %s", sp.capsOk ? "present" : "MISSING");
            }
        }
    }
}
