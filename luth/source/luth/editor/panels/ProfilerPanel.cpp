#include "luthpch.h"
#include "ProfilerPanel.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/Time.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/scene/Systems.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/resources/AssetManager.h"
#include "luth/utils/LuthIcons.h"
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

        if (absBytes >= 1024.0 * 1024.0)
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

                // Min/max/avg over history
                m_FrameTimeMin = *std::min_element(m_FrameTimeHistory.begin(), m_FrameTimeHistory.end());
                m_FrameTimeMax = *std::max_element(m_FrameTimeHistory.begin(), m_FrameTimeHistory.end());
                float sum = 0.0f;
                for (float v : m_FrameTimeHistory) sum += v;
                m_FrameTimeAvg = sum / (float)m_FrameTimeHistory.size();

                // Memory snapshot
                m_MemSnapshot = Memory::MemoryTracker::GetSnapshot();
                std::rotate(m_MemoryHistory.begin(), m_MemoryHistory.begin() + 1, m_MemoryHistory.end());
                m_MemoryHistory.back() = (float)m_MemSnapshot.TotalCurrent / (1024.0f * 1024.0f);

                // GPU stats
                m_GPUStats = VulkanAllocator::GetStats();

                // Trim feedback countdown
                if (m_TrimFeedbackTimer > 0.0f)
                    m_TrimFeedbackTimer -= 0.1f;
            }

            // ================================================================
            // Overview Header
            // ================================================================
            if (UI::BeginCollapsingHeader("Overview", true))
            {
                    // FPS & frame time
                    ImGui::TextColored(FrameTimeColor(m_FrameTime), "FPS: %.1f", m_FPS);
                    ImGui::SameLine();
                    ImGui::TextColored(FrameTimeColor(m_FrameTime), "  Frame Time: %.3f ms", m_FrameTime);

                    // Frame time graph
                    char overlay[32];
                    snprintf(overlay, sizeof(overlay), "avg %.1f ms", m_FrameTimeAvg);
                    ImGui::PlotLines("##FrameTimes", m_FrameTimeHistory.data(), (int)m_FrameTimeHistory.size(),
                        0, overlay, 0.0f, 33.0f, ImVec2(0, 50));

                    // Min/max/avg annotations
                    ImGui::TextColored(FrameTimeColor(m_FrameTimeMin), "Min: %.1f ms", m_FrameTimeMin);
                    ImGui::SameLine();
                    ImGui::TextColored(FrameTimeColor(m_FrameTimeMax), "  Max: %.1f ms", m_FrameTimeMax);
                    ImGui::SameLine();
                    ImGui::TextColored(FrameTimeColor(m_FrameTimeAvg), "  Avg: %.1f ms", m_FrameTimeAvg);

                    ImGui::Separator();

                    // Memory summary
                    char buf1[64], buf2[64];
                    ImGui::Text("Memory: %s (Peak: %s)",
                        FormatBytes(m_MemSnapshot.TotalCurrent, buf1, sizeof(buf1)),
                        FormatBytes(m_MemSnapshot.TotalPeak, buf2, sizeof(buf2)));

                    // GPU summary
                    float usedMB = (float)m_GPUStats.UsedBytes / (1024.0f * 1024.0f);
                    float freeMB = (float)m_GPUStats.FreeBytes / (1024.0f * 1024.0f);
                    float totalMB = usedMB + freeMB;
                    ImGui::Text("GPU: %.1f / %.1f MB", usedMB, totalMB);
                    if (totalMB > 0.0f)
                        ColoredProgressBar(usedMB / totalMB, ImVec2(-1, 0), nullptr);

                    ImGui::Separator();

                    // Job system summary
                    JobSystem::Stats stats = JobSystem::GetStats();
                    ImGui::Text("Jobs Queued: %d  |  Fibers: %d/%d",
                        stats.HighQueueSize, stats.TotalFibers - stats.FreeFibers, stats.TotalFibers);

                UI::EndCollapsingHeader();
            }

            // ============================================================
            // CPU Header
            // ============================================================
            if (UI::BeginCollapsingHeader("CPU"))
            {
                    JobSystem::Stats stats = JobSystem::GetStats();

                    if (UI::BeginInfoTable("JobStats")) {
                        UI::InfoRow("Worker Threads", "%d", stats.ThreadCount);
                        UI::InfoRow("Active Fibers", "%d / %d", stats.TotalFibers - stats.FreeFibers, stats.TotalFibers);
                        UI::InfoRow("Peak Fibers", "%d", stats.PeakFibers);
                        UI::InfoRow("Queued Jobs", "%d", stats.HighQueueSize);
                        UI::EndInfoTable();
                    }

                    ImGui::Separator();

                    // Queue load
                    ImGui::Text("Queue Load");
                    float queueRatio = (float)stats.HighQueueSize / 100.0f;
                    ColoredProgressBar(queueRatio, ImVec2(-1, 0), nullptr);

                    // Fiber pool
                    ImGui::Text("Fiber Pool");
                    float fiberRatio = (float)(stats.TotalFibers - stats.FreeFibers) / (float)stats.TotalFibers;
                    char fiberOverlay[32];
                    snprintf(fiberOverlay, sizeof(fiberOverlay), "%d/%d", stats.TotalFibers - stats.FreeFibers, stats.TotalFibers);
                    ColoredProgressBar(fiberRatio, ImVec2(-1, 0), fiberOverlay);

                    ImGui::Separator();

                    // Worker thread grid
                    ImGui::Text("Workers");
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    const float squareSize = 16.0f;
                    const float spacing = 2.0f;

                    for (u32 i = 0; i < stats.ThreadCount; ++i)
                    {
                        float x = cursor.x + i * (squareSize + spacing);
                        float y = cursor.y;

                        // All workers currently report "Running" — green
                        ImU32 color = IM_COL32(138, 219, 0, 255);

                        drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + squareSize, y + squareSize), color, 2.0f);

                        // Invisible button for tooltip
                        ImGui::SetCursorScreenPos(ImVec2(x, y));
                        char btnId[16];
                        snprintf(btnId, sizeof(btnId), "##w%d", i);
                        ImGui::InvisibleButton(btnId, ImVec2(squareSize, squareSize));
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::Text("Worker %d: Running", i);
                            ImGui::EndTooltip();
                        }
                        if (i < stats.ThreadCount - 1)
                            ImGui::SameLine(0.0f, spacing);
                    }

                    // Advance cursor past the grid
                    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + squareSize + spacing));
                    ImGui::Dummy(ImVec2(0, 0));

                UI::EndCollapsingHeader();
            }

            // ============================================================
            // Memory Header
            // ============================================================
            if (UI::BeginCollapsingHeader("Memory"))
            {
                    char buf1[64], buf2[64];

                    // Total tracked memory
                    ImGui::Text("Total Tracked: %s (Peak: %s)",
                        FormatBytes(m_MemSnapshot.TotalCurrent, buf1, sizeof(buf1)),
                        FormatBytes(m_MemSnapshot.TotalPeak, buf2, sizeof(buf2)));

                    // Memory graph
                    ImGui::PlotLines("##MemoryUsage", m_MemoryHistory.data(), (int)m_MemoryHistory.size(),
                        0, "Total MB", 0.0f, 0.0f, ImVec2(0, 40));

                    ImGui::Separator();

                    // Per-category table
                    if (ImGui::BeginTable("MemCategories", 5,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
                    {
                        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_None, 2.0f);
                        ImGui::TableSetupColumn("Current",  ImGuiTableColumnFlags_None, 1.5f);
                        ImGui::TableSetupColumn("Peak",     ImGuiTableColumnFlags_None, 1.5f);
                        ImGui::TableSetupColumn("Alloc",   ImGuiTableColumnFlags_None, 1.0f);
                        ImGui::TableSetupColumn("Free",    ImGuiTableColumnFlags_None, 1.0f);
                        ImGui::TableHeadersRow();

                        for (u8 i = 0; i < static_cast<u8>(Memory::Category::Count); ++i)
                        {
                            auto& entry = m_MemSnapshot.Categories[i];

                            if (entry.Allocs == 0 && entry.Current == 0)
                                continue;

                            ImGui::TableNextRow();

                            bool potentialLeak = (entry.Current > 0) && (entry.Frees < entry.Allocs);

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", Memory::MemoryTracker::GetCategoryName(static_cast<Memory::Category>(i)));

                            ImGui::TableNextColumn();
                            if (entry.Current < 0)
                                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", FormatBytes(entry.Current, buf1, sizeof(buf1)));
                            else
                                ImGui::Text("%s", FormatBytes(entry.Current, buf1, sizeof(buf1)));

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", FormatBytes(entry.Peak, buf1, sizeof(buf1)));

                            ImGui::TableNextColumn();
                            ImGui::Text("%u", entry.Allocs);

                            ImGui::TableNextColumn();
                            if (potentialLeak)
                                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%u", entry.Frees);
                            else
                                ImGui::Text("%u", entry.Frees);
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
                    {
                        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Evicted %u assets", m_LastTrimCount);
                    }

                UI::EndCollapsingHeader();
            }

            // ============================================================
            // GPU Header
            // ============================================================
            if (UI::BeginCollapsingHeader("GPU"))
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
