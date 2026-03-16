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

#include <imgui.h>

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
            // Update Timers
            m_UpdateTimer += Time::UnscaledDeltaTime();
            if (m_UpdateTimer >= 0.1f) // Update UI 10 times a second
            {
                m_UpdateTimer = 0.0f;
                m_FPS = ImGui::GetIO().Framerate;
                m_FrameTime = 1000.0f / m_FPS;

                // Shift frame time history
                std::rotate(m_FrameTimeHistory.begin(), m_FrameTimeHistory.begin() + 1, m_FrameTimeHistory.end());
                m_FrameTimeHistory.back() = m_FrameTime;

                // Update memory snapshot
                m_MemSnapshot = Memory::MemoryTracker::GetSnapshot();
                std::rotate(m_MemoryHistory.begin(), m_MemoryHistory.begin() + 1, m_MemoryHistory.end());
                m_MemoryHistory.back() = (float)m_MemSnapshot.TotalCurrent / (1024.0f * 1024.0f);
            }

            // ================================================================
            // 1. Performance Graph
            // ================================================================
            if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text("FPS: %.1f", m_FPS);
                ImGui::Text("Frame Time: %.3f ms", m_FrameTime);

                ImGui::PlotLines("##FrameTimes", m_FrameTimeHistory.data(), (int)m_FrameTimeHistory.size(), 0, nullptr, 0.0f, 33.0f, ImVec2(0, 40));
            }

            // ================================================================
            // 2. Job System
            // ================================================================
            if (ImGui::CollapsingHeader("Job System", ImGuiTreeNodeFlags_DefaultOpen))
            {
                JobSystem::Stats stats = JobSystem::GetStats();

                ImGui::Text("Worker Threads: %d", stats.ThreadCount);
                ImGui::Text("Active Fibers: %d / %d", stats.TotalFibers - stats.FreeFibers, stats.TotalFibers);
                ImGui::Text("Peak Fibers (Frame): %d", stats.PeakFibers);
                ImGui::Text("Queued Jobs: %d", stats.HighQueueSize);

                ImGui::Separator();

                // Queue Load
                float queueRatio = (float)stats.HighQueueSize / 100.0f;
                ImGui::ProgressBar(queueRatio, ImVec2(-1, 0.0f), "Queue Load");

                // Fiber Pool Usage
                float fiberRatio = (float)(stats.TotalFibers - stats.FreeFibers) / (float)stats.TotalFibers;
                char fiberOverlay[32];
                sprintf(fiberOverlay, "%d/%d", stats.TotalFibers - stats.FreeFibers, stats.TotalFibers);
                ImGui::ProgressBar(fiberRatio, ImVec2(-1, 0.0f), fiberOverlay);
                ImGui::SameLine();
                ImGui::Text("Fiber Pool");

                if (ImGui::TreeNode("Worker Threads"))
                {
                    for (u32 i = 0; i < stats.ThreadCount; ++i)
                    {
                        ImGui::Text("Worker %d: Running", i);
                    }
                    ImGui::TreePop();
                }
            }

            // ================================================================
            // 3. Memory
            // ================================================================
            if (ImGui::CollapsingHeader("Memory", ImGuiTreeNodeFlags_DefaultOpen))
            {
                char buf1[64], buf2[64];

                // -- Total Tracked Memory --
                ImGui::Text("Total Tracked: %s (Peak: %s)",
                    FormatBytes(m_MemSnapshot.TotalCurrent, buf1, sizeof(buf1)),
                    FormatBytes(m_MemSnapshot.TotalPeak, buf2, sizeof(buf2)));

                // Memory graph
                ImGui::PlotLines("##MemoryUsage", m_MemoryHistory.data(), (int)m_MemoryHistory.size(),
                    0, "Total MB", 0.0f, 0.0f, ImVec2(0, 40));

                ImGui::Separator();

                // -- Per-Category Table --
                if (ImGui::BeginTable("MemCategories", 5,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_None, 2.0f);
                    ImGui::TableSetupColumn("Current",  ImGuiTableColumnFlags_None, 1.5f);
                    ImGui::TableSetupColumn("Peak",     ImGuiTableColumnFlags_None, 1.5f);
                    ImGui::TableSetupColumn("Allocs",   ImGuiTableColumnFlags_None, 1.0f);
                    ImGui::TableSetupColumn("Frees",    ImGuiTableColumnFlags_None, 1.0f);
                    ImGui::TableHeadersRow();

                    for (u8 i = 0; i < static_cast<u8>(Memory::Category::Count); ++i)
                    {
                        auto& entry = m_MemSnapshot.Categories[i];

                        // Skip categories with no activity
                        if (entry.Allocs == 0 && entry.Current == 0)
                            continue;

                        ImGui::TableNextRow();

                        // Highlight rows with potential leaks (current != 0 and frees < allocs)
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

                // -- GPU Memory (VMA) --
                auto gpuStats = VulkanAllocator::GetStats();
                float usedMB = (float)gpuStats.UsedBytes / (1024.0f * 1024.0f);
                float freeMB = (float)gpuStats.FreeBytes / (1024.0f * 1024.0f);
                float totalMB = usedMB + freeMB;

                ImGui::Text("GPU Memory (VMA)");
                ImGui::Text("  Used: %.2f MB  |  Free: %.2f MB  |  Allocations: %d",
                    usedMB, freeMB, gpuStats.AllocationCount);

                if (totalMB > 0.0f)
                    ImGui::ProgressBar(usedMB / totalMB, ImVec2(-1, 0));

                ImGui::Separator();

                if (ImGui::Button("Trim Unused Assets"))
                {
                    AssetManager::Trim();
                }
            }
        }
        ImGui::End();
        ImGui::PopFont();
    }
}
