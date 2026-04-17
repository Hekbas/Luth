#include "luthpch.h"
#include "ProfilerPanel.h"
#include "luth/jobs/JobSystem.h"
#include "luth/editor/EditorColors.h"
#include "luth/core/Time.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/resources/AssetManager.h"
#include "luth/editor/widgets/Icons.h"
#include "luth/editor/Editor.h"
#include "luth/editor/UI.h"

#include <imgui.h>
#include <algorithm>

namespace
{
    void ColoredProgressBar(float ratio, const ImVec2& size, const char* overlay)
    {
        ImVec4 color;
        if (ratio < 0.50f)
            color = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        else if (ratio < 0.75f)
            color = ImVec4(0.9f, 0.8f, 0.1f, 1.0f);
        else
            color = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
        ImGui::ProgressBar(ratio, size, overlay);
        ImGui::PopStyleColor();
    }

    ImVec4 FrameTimeColor(float ms)
    {
        if (ms < 16.0f)  return ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        if (ms < 33.0f)  return ImVec4(0.9f, 0.8f, 0.1f, 1.0f);
        return ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
    }
}

namespace Luth
{
    ProfilerPanel::ProfilerPanel()
    {
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

        // 1 GB = 1024 * 1024 * 1024 bytes
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

    void ProfilerPanel::OnRender()
    {
        ImGui::PushFont(Editor::GetFASolid());
        std::string title = ICON_FA_CHART_LINE + std::string("  Profiler");

        if (ImGui::Begin(title.c_str()))
        {
            // ================================================================
            // Per-frame sampling (worker states + job counters)
            // ================================================================
            {
                JobSystem::Stats stats = JobSystem::GetStats();
                m_WorkerThreadCount = stats.ThreadCount;
                m_CachedJobsExecuted = stats.JobsExecuted;
                m_CachedStealSuccesses = stats.StealSuccesses;

                for (u32 i = 0; i < stats.ThreadCount && i < JobSystem::MAX_WORKER_THREADS; ++i)
                    m_WorkerStateHistory[i][m_WorkerHistoryHead] = stats.PerThreadState[i];
                m_WorkerHistoryHead = (m_WorkerHistoryHead + 1) % WORKER_HISTORY_FRAMES;

                // GPU frame time from render graph snapshot
                if (auto rs = SystemRegistry::GetSystem<RenderingSystem>())
                    m_GPUFrameTimeMs = rs->GetGraphSnapshot().totalGpuTimeMs;
            }

            // ================================================================
            // Update cached stats at 10Hz
            // ================================================================
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

                m_MemSnapshot = Memory::MemoryTracker::GetSnapshot();
                std::rotate(m_MemoryHistory.begin(), m_MemoryHistory.begin() + 1, m_MemoryHistory.end());
                m_MemoryHistory.back() = (float)m_MemSnapshot.TotalCurrent / (1024.0f * 1024.0f);

                m_GPUStats = VulkanAllocator::GetStats();

                if (m_TrimFeedbackTimer > 0.0f)
                    m_TrimFeedbackTimer -= 0.1f;
            }

            // Helper lambdas
            auto WorkerStateColor = [](JobSystem::WorkerState state) -> ImU32 {
                switch (state) {
                    case JobSystem::WorkerState::Running:  return EditorColors::WorkerRunning;
                    case JobSystem::WorkerState::Stealing: return EditorColors::WorkerStealing;
                    case JobSystem::WorkerState::Sleeping: return EditorColors::WorkerSleeping;
                    default:                               return EditorColors::WorkerIdle;
                }
            };

            auto WorkerStateName = [](JobSystem::WorkerState state) -> const char* {
                switch (state) {
                    case JobSystem::WorkerState::Running:  return "Running";
                    case JobSystem::WorkerState::Stealing: return "Stealing";
                    case JobSystem::WorkerState::Sleeping: return "Sleeping";
                    default:                               return "Idle";
                }
            };

            // ================================================================
            // Overview — CPU/GPU Frame Budget Bars
            // ================================================================
            if (UI::BeginCollapsingHeader("Overview", true))
            {
                // Align text vertically with the adjacent input field for visual consistency.
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("FPS Target");
                ImGui::SameLine();

                // Configure a draggable integer input for the target FPS.
                // Conditionally format the display string to show "None" when the target is 0.
                ImGui::PushItemWidth(50.0f);
                const char* format = (m_TargetFPS == 0) ? "None" : "%d";
                if (ImGui::DragInt("##TargetFPS", &m_TargetFPS, 1.0f, 0, 999, format))
                {
                    // Clamp the target FPS to prevent invalid negative values.
                    m_TargetFPS = std::max(m_TargetFPS, 0);

                    // Recalculate the frame budget (in milliseconds). 
                    // Default the visual budget line to 16.67ms (60 FPS) if no target is specified.
                    m_FrameBudgetMs = m_TargetFPS > 0 ? 1000.0f / (float)m_TargetFPS : 16.67f; 
                }
                ImGui::PopItemWidth();

                // Format the current FPS for the right-aligned live statistics readout.
                char liveStatsBuf[64];
                snprintf(liveStatsBuf, sizeof(liveStatsBuf), "%.0f", m_FPS);

                // Calculate the required horizontal offset to right-align the statistics text.
                float textWidth = ImGui::CalcTextSize(liveStatsBuf).x;
                float spaceAvailable = ImGui::GetContentRegionAvail().x;

                // Note: If the parent container or graph utilizes a fixed width, 
                // `spaceAvailable` should be replaced with that specific metric.
                if (spaceAvailable > textWidth)
                {
                    ImGui::SameLine(spaceAvailable - textWidth);
                }
                else
                {
                    // Fallback to standard inline placement if layout space is constrained.
                    ImGui::SameLine(); 
                }

                // Render the statistics text, applying a dynamic color based on current frame time performance.
                ImGui::TextColored(FrameTimeColor(m_FrameTime), "%s", liveStatsBuf);

                // CPU/GPU budget bars
                float barHeight = 20.0f;
                float availWidth = ImGui::GetContentRegionAvail().x;
                ImDrawList* drawList = ImGui::GetWindowDrawList();

                // CPU bar
                {
                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    float ratio = m_FrameTime / m_FrameBudgetMs;
                    float barWidth = std::min(ratio, 1.5f) / 1.5f * availWidth;
                    bool overBudget = m_TargetFPS > 0 && m_FrameTime > m_FrameBudgetMs;
                    ImU32 barColor = overBudget ? IM_COL32(200, 60, 60, 255) : IM_COL32(80, 180, 80, 255);

                    // Background
                    drawList->AddRectFilled(cursor, ImVec2(cursor.x + availWidth, cursor.y + barHeight), IM_COL32(40, 40, 40, 255), 2.0f);
                    // Fill
                    drawList->AddRectFilled(cursor, ImVec2(cursor.x + barWidth, cursor.y + barHeight), barColor, 2.0f);
                    // Budget line
                    if (m_TargetFPS > 0) {
                        float budgetX = cursor.x + (1.0f / 1.5f) * availWidth;
                        drawList->AddLine(ImVec2(budgetX, cursor.y), ImVec2(budgetX, cursor.y + barHeight), IM_COL32(255, 255, 255, 180), 1.0f);
                    }
                    // Label
                    char cpuLabel[32];
                    snprintf(cpuLabel, sizeof(cpuLabel), "CPU: %.1f ms", m_FrameTime);
                    drawList->AddText(ImVec2(cursor.x + 4, cursor.y + 2), IM_COL32(255, 255, 255, 255), cpuLabel);

                    ImGui::Dummy(ImVec2(availWidth, barHeight + 2));
                }

                // GPU bar
                {
                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    float gpuTime = m_GPUFrameTimeMs > 0.0f ? m_GPUFrameTimeMs : 0.0f;
                    float ratio = gpuTime / m_FrameBudgetMs;
                    float barWidth = std::min(ratio, 1.5f) / 1.5f * availWidth;
                    bool overBudget = m_TargetFPS > 0 && gpuTime > m_FrameBudgetMs;
                    ImU32 barColor = overBudget ? IM_COL32(200, 60, 60, 255) : IM_COL32(60, 120, 200, 255);

                    drawList->AddRectFilled(cursor, ImVec2(cursor.x + availWidth, cursor.y + barHeight), IM_COL32(40, 40, 40, 255), 2.0f);
                    drawList->AddRectFilled(cursor, ImVec2(cursor.x + barWidth, cursor.y + barHeight), barColor, 2.0f);
                    if (m_TargetFPS > 0) {
                        float budgetX = cursor.x + (1.0f / 1.5f) * availWidth;
                        drawList->AddLine(ImVec2(budgetX, cursor.y), ImVec2(budgetX, cursor.y + barHeight), IM_COL32(255, 255, 255, 180), 1.0f);
                    }
                    char gpuLabel[32];
                    snprintf(gpuLabel, sizeof(gpuLabel), "GPU: %.1f ms", gpuTime);
                    drawList->AddText(ImVec2(cursor.x + 4, cursor.y + 2), IM_COL32(255, 255, 255, 255), gpuLabel);

                    ImGui::Dummy(ImVec2(availWidth, barHeight + 2));
                }

                ImGui::Separator();
                
                float graphWidth = ImGui::GetContentRegionAvail().x;
                
                // Frame time graph
                ImGui::PlotLines("##FrameTimes", m_FrameTimeHistory.data(), (int)m_FrameTimeHistory.size(),
                    0, nullptr, 0.0f, m_FrameBudgetMs * 2.0f, ImVec2(graphWidth, 40));

                ImGui::TextColored(FrameTimeColor(m_FrameTimeMin), "Min: %.1f ms", m_FrameTimeMin);
                ImGui::SameLine();
                ImGui::TextColored(FrameTimeColor(m_FrameTimeAvg), "  Avg: %.1f ms", m_FrameTimeAvg);
                ImGui::SameLine();
                ImGui::TextColored(FrameTimeColor(m_FrameTimeMax), "  Max: %.1f ms", m_FrameTimeMax);

                UI::EndCollapsingHeader();
            }

            // ============================================================
            // CPU — Worker Timeline Swimlanes
            // ============================================================
            if (UI::BeginCollapsingHeader("CPU"))
            {
                // --- Summary Line with Right-Aligned Legend Tooltip ---
                char summaryBuf[128];
                snprintf(summaryBuf, sizeof(summaryBuf), "%d workers  |  %d jobs  |  %d steals",
                    m_WorkerThreadCount, m_CachedJobsExecuted, m_CachedStealSuccesses);
                    
                ImGui::TextDisabled("%s", summaryBuf);

                // Calculate space to right-align the legend marker
                const char* legendMarker = "(?)";
                float markerWidth = ImGui::CalcTextSize(legendMarker).x;
                float spaceAvailable = ImGui::GetContentRegionAvail().x;

                if (spaceAvailable > markerWidth)
                {
                    ImGui::SameLine(ImGui::GetCursorPosX() + spaceAvailable - markerWidth);
                }
                else
                {
                    ImGui::SameLine(); 
                }

                ImGui::TextDisabled("%s", legendMarker);
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Worker States");
                    ImGui::Separator();

                    if (ImGui::BeginTable("WorkerLegendTable", 2, ImGuiTableFlags_SizingStretchProp))
                    {
                        ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 14.0f);
                        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);

                        struct LegendItem { const char* name; ImU32 color; };
                        const LegendItem legendData[] = {
                            { "Running",  Luth::EditorColors::WorkerRunning },
                            { "Stealing", Luth::EditorColors::WorkerStealing },
                            { "Idle",     Luth::EditorColors::WorkerIdle },
                            { "Sleeping", Luth::EditorColors::WorkerSleeping }
                        };

                        float textLineHeight = ImGui::GetTextLineHeight();
                        float colorBoxSize = textLineHeight * 0.8f;
                        float verticalOffset = (textLineHeight - colorBoxSize) * 0.5f;

                        for (int i = 0; i < 4; ++i)
                        {
                            ImGui::TableNextRow();
                            ImGui::PushID(i);

                            // Column 0: Color Box
                            ImGui::TableSetColumnIndex(0);
                            float startY = ImGui::GetCursorPosY();
                            ImGui::SetCursorPosY(startY + verticalOffset);
                            
                            ImVec4 col = ImGui::ColorConvertU32ToFloat4(legendData[i].color);
                            ImGui::ColorButton("##wc", col, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(colorBoxSize, colorBoxSize));

                            // Column 1: State Name
                            ImGui::TableSetColumnIndex(1);
                            ImGui::SetCursorPosY(startY);
                            ImGui::TextUnformatted(legendData[i].name);

                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTooltip();
                }

                ImGui::Separator();

                // --- Timeline Rendering ---
                float availWidth = ImGui::GetContentRegionAvail().x;
                float segWidth = availWidth / (float)WORKER_HISTORY_FRAMES;
                if (segWidth < 1.0f) segWidth = 1.0f;
                ImDrawList* drawList = ImGui::GetWindowDrawList();

                const float mainBarHeight = 20.0f;
                const float workerBarHeight = 8.0f;
                const float workerGap = 1.0f;

                // Main thread bar (worker 0)
                ImGui::Text("Main Thread");
                {
                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    for (u32 f = 0; f < WORKER_HISTORY_FRAMES; ++f)
                    {
                        u32 histIdx = (m_WorkerHistoryHead + f) % WORKER_HISTORY_FRAMES;
                        float x = cursor.x + f * segWidth;
                        ImU32 color = WorkerStateColor(m_WorkerStateHistory[0][histIdx]);
                        drawList->AddRectFilled(ImVec2(x, cursor.y), ImVec2(x + segWidth, cursor.y + mainBarHeight), color);
                    }
                    ImGui::InvisibleButton("##main_bar", ImVec2(availWidth, mainBarHeight));
                    if (ImGui::IsItemHovered())
                    {
                        float mouseX = ImGui::GetMousePos().x - ImGui::GetItemRectMin().x;
                        u32 frame = (u32)(mouseX / segWidth);
                        if (frame < WORKER_HISTORY_FRAMES)
                        {
                            u32 histIdx = (m_WorkerHistoryHead + frame) % WORKER_HISTORY_FRAMES;
                            ImGui::SetTooltip("Main Thread: %s", WorkerStateName(m_WorkerStateHistory[0][histIdx]));
                        }
                    }
                }

                ImGui::Dummy(ImVec2(0, 4));

                // Worker bars (1..N-1)
                if (m_WorkerThreadCount > 1)
                {
                    ImGui::Text("Workers");
                    ImVec2 startCursor = ImGui::GetCursorScreenPos();
                    float totalHeight = (m_WorkerThreadCount - 1) * (workerBarHeight + workerGap);

                    for (u32 w = 1; w < m_WorkerThreadCount && w < JobSystem::MAX_WORKER_THREADS; ++w)
                    {
                        float yOffset = (w - 1) * (workerBarHeight + workerGap);
                        for (u32 f = 0; f < WORKER_HISTORY_FRAMES; ++f)
                        {
                            u32 histIdx = (m_WorkerHistoryHead + f) % WORKER_HISTORY_FRAMES;
                            float x = startCursor.x + f * segWidth;
                            float y = startCursor.y + yOffset;
                            ImU32 color = WorkerStateColor(m_WorkerStateHistory[w][histIdx]);
                            drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + segWidth, y + workerBarHeight), color);
                        }
                    }

                    ImGui::InvisibleButton("##worker_bars", ImVec2(availWidth, totalHeight));
                    if (ImGui::IsItemHovered())
                    {
                        float mouseX = ImGui::GetMousePos().x - ImGui::GetItemRectMin().x;
                        float mouseY = ImGui::GetMousePos().y - ImGui::GetItemRectMin().y;
                        u32 frame = (u32)(mouseX / segWidth);
                        u32 worker = 1 + (u32)(mouseY / (workerBarHeight + workerGap));
                        if (frame < WORKER_HISTORY_FRAMES && worker < m_WorkerThreadCount)
                        {
                            u32 histIdx = (m_WorkerHistoryHead + frame) % WORKER_HISTORY_FRAMES;
                            ImGui::SetTooltip("Worker %d: %s", worker, WorkerStateName(m_WorkerStateHistory[worker][histIdx]));
                        }
                    }
                }

                UI::EndCollapsingHeader();
            }

            // ============================================================
            // Memory — Stacked Bar Chart
            // ============================================================
            if (UI::BeginCollapsingHeader("Memory"))
            {
                char buf1[64], buf2[64];
                ImGui::Text("Total: %s (Peak: %s)",
                    FormatBytes(m_MemSnapshot.TotalCurrent, buf1, sizeof(buf1)),
                    FormatBytes(m_MemSnapshot.TotalPeak, buf2, sizeof(buf2)));

                // Memory history graph
                // 1. Find the current peak memory usage
                float maxMem = 0.0f;
                for (float mem : m_MemoryHistory)
                {
                    maxMem = std::max(mem, maxMem);
                }

                // 2. Add a small buffer. 
                // If your memory hits exactly 128MB, this pushes it slightly over 
                // so the math below snaps the ceiling to 256MB, giving you headroom.
                float targetMax = maxMem * 1.2f;

                // 3. Snap to the next power of 2 using base-2 math
                // e.g., if targetMax is 140: log2(140) = ~7.12 -> ceil(7.12) = 8 -> exp2(8) = 256
                float dynamicMax = std::exp2(std::ceil(std::log2(targetMax)));

                // 4. Enforce your absolute minimum baseline of 64 MB
                dynamicMax = std::max(64.0f, dynamicMax);
                
                // 5. Fetch the exact width available in the current container/panel
                float graphWidth = ImGui::GetContentRegionAvail().x;
                
                // 6. Render the plot
                ImGui::PlotLines("##MemoryUsage", 
                    m_MemoryHistory.data(), 
                    (int)m_MemoryHistory.size(),
                    0, 
                    nullptr,     
                    0.0f,           // scale_min: anchor to 0
                    dynamicMax,     // scale_max: snaps to 64, 128, 256, 512, etc.
                    ImVec2(graphWidth, 40));

                ImGui::Separator();
            
                // Category color lookup
                const ImU32 categoryColors[] = {
                    EditorColors::MemGeneral,
                    EditorColors::MemRendering,
                    EditorColors::MemScene,
                    EditorColors::MemJobs,
                    EditorColors::MemResources,
                    EditorColors::MemEditor,
                    EditorColors::MemFrameLinear,
                    EditorColors::MemFrameTagged,
                    EditorColors::MemGPU
                };

                // Stacked horizontal bar
                float barHeight = 20.0f;
                float availWidth = ImGui::GetContentRegionAvail().x;
                ImVec2 cursor = ImGui::GetCursorScreenPos();
                ImDrawList* drawList = ImGui::GetWindowDrawList();

                // Background
                drawList->AddRectFilled(cursor, ImVec2(cursor.x + availWidth, cursor.y + barHeight), IM_COL32(40, 40, 40, 255), 2.0f);

                i64 totalBytes = m_MemSnapshot.TotalCurrent > 0 ? m_MemSnapshot.TotalCurrent : 1;
                float xOffset = 0.0f;

                struct SegmentInfo { u8 catIndex; float x; float width; };
                SegmentInfo segments[9]{};
                u32 segCount = 0;

                for (u8 i = 0; i < static_cast<u8>(Memory::Category::Count); ++i)
                {
                    i64 current = m_MemSnapshot.Categories[i].Current;
                    if (current <= 0) continue;

                    float segWidth = ((float)current / (float)totalBytes) * availWidth;
                    if (segWidth < 1.0f) segWidth = 1.0f;

                    drawList->AddRectFilled(
                        ImVec2(cursor.x + xOffset, cursor.y),
                        ImVec2(cursor.x + xOffset + segWidth, cursor.y + barHeight),
                        categoryColors[i]);

                    segments[segCount++] = { i, xOffset, segWidth };
                    xOffset += segWidth;
                }

                // Invisible button for tooltip
                ImGui::InvisibleButton("##membar", ImVec2(availWidth, barHeight));
                if (ImGui::IsItemHovered())
                {
                    float mouseX = ImGui::GetMousePos().x - ImGui::GetItemRectMin().x;
                    for (u32 s = 0; s < segCount; ++s)
                    {
                        if (mouseX >= segments[s].x && mouseX < segments[s].x + segments[s].width)
                        {
                            i64 current = m_MemSnapshot.Categories[segments[s].catIndex].Current;
                            float pct = (float)current / (float)totalBytes * 100.0f;
                            ImGui::SetTooltip("%s: %s (%.1f%%)",
                                Memory::MemoryTracker::GetCategoryName(static_cast<Memory::Category>(segments[s].catIndex)),
                                FormatBytes(current, buf1, sizeof(buf1)), pct);
                            break;
                        }
                    }
                }

                // Legend (colored dots + name + size)
                ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("MemoryLegendTable", 2, tableFlags))
                {
                    // Column 0 (Name) takes up the remaining space, Column 1 (Size) has a fixed minimum width
                    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);

                    // Calculate dynamic sizing for the color box so it scales safely with UI font sizes
                    float textLineHeight = ImGui::GetTextLineHeight();
                    float colorBoxSize = textLineHeight * 0.8f; 
                    float verticalOffset = (textLineHeight - colorBoxSize) * 0.5f;

                    for (u32 s = 0; s < segCount; ++s)
                    {
                        ImGui::PushID(s);
                        ImGui::TableNextRow();

                        u8 catIdx = segments[s].catIndex;
                        ImVec4 col = ImGui::ColorConvertU32ToFloat4(categoryColors[catIdx]);

                        // --- Column 0: Color Box + Category Name ---
                        ImGui::TableSetColumnIndex(0);

                        // Push cursor down slightly to vertically center the color box with the text
                        float startY = ImGui::GetCursorPosY();
                        ImGui::SetCursorPosY(startY + verticalOffset);
                        
                        ImGui::ColorButton("##mc", col, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(colorBoxSize, colorBoxSize));
                        
                        ImGui::SameLine();
                        ImGui::SetCursorPosY(startY); // Reset Y cursor back to normal for the text
                        ImGui::TextUnformatted(Memory::MemoryTracker::GetCategoryName(static_cast<Memory::Category>(catIdx)));

                        // --- Column 1: Size (Right-aligned) ---
                        ImGui::TableSetColumnIndex(1);
                        
                        char buf1[32];
                        const char* sizeText = FormatBytes(m_MemSnapshot.Categories[catIdx].Current, buf1, sizeof(buf1));
                        
                        // Calculate the width of the text to push it cleanly to the right edge of the column
                        float textWidth = ImGui::CalcTextSize(sizeText).x;
                        float columnWidth = ImGui::GetColumnWidth();
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (columnWidth - textWidth));
                        
                        ImGui::TextUnformatted(sizeText);

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                ImGui::Separator();

                // Trim buttons
                if (ImGui::Button("Trim Unused Assets"))
                {
                    m_LastTrimCount = AssetManager::Trim(false);
                    m_TrimFeedbackTimer = 3.0f;
                }
                ImGui::SameLine();
                if (ImGui::Button("Force Trim"))
                {
                    m_LastTrimCount = AssetManager::Trim(true);
                    m_TrimFeedbackTimer = 3.0f;
                }

                if (m_TrimFeedbackTimer > 0.0f)
                    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Evicted %u assets", m_LastTrimCount);

                UI::EndCollapsingHeader();
            }

            // ============================================================
            // GPU Memory
            // ============================================================
            if (UI::BeginCollapsingHeader("GPU Memory"))
            {
                float usedMB  = (float)m_GPUStats.UsedBytes / (1024.0f * 1024.0f);
                float freeMB  = (float)m_GPUStats.FreeBytes / (1024.0f * 1024.0f);
                float totalMB = usedMB + freeMB;

                if (UI::BeginInfoTable("GPUMem")) {
                    UI::InfoRow("Used",        "%.2f MB", usedMB);
                    UI::InfoRow("Free",        "%.2f MB", freeMB);
                    UI::InfoRow("Total",       "%.2f MB", totalMB);
                    UI::InfoRow("Allocations", "%u", m_GPUStats.AllocationCount);
                    UI::InfoRow("Blocks",      "%u", m_GPUStats.BlockCount);
                    UI::EndInfoTable();
                }

                if (totalMB > 0.0f)
                    ColoredProgressBar(usedMB / totalMB, ImVec2(-1, 0), nullptr);

                UI::EndCollapsingHeader();
            }
        }
        ImGui::End();
        ImGui::PopFont();
    }
}
