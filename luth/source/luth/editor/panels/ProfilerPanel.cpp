#include "luthpch.h"
#include "ProfilerPanel.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Time.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/ECS/Systems.h"
#include "luth/ECS/systems/RenderingSystem.h"
#include "luth/utils/LuthIcons.h"

#include <imgui.h>

namespace Luth
{
    ProfilerPanel::ProfilerPanel()
    {
        m_FrameTimeHistory.resize(100, 0.0f);
    }

    void ProfilerPanel::OnInit()
    {
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
                
                // Shift history
                std::rotate(m_FrameTimeHistory.begin(), m_FrameTimeHistory.begin() + 1, m_FrameTimeHistory.end());
                m_FrameTimeHistory.back() = m_FrameTime;
            }

            // 1. Performance Graph
            if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text("FPS: %.1f", m_FPS);
                ImGui::Text("Frame Time: %.3f ms", m_FrameTime);
                
                ImGui::PlotLines("##FrameTimes", m_FrameTimeHistory.data(), (int)m_FrameTimeHistory.size(), 0, nullptr, 0.0f, 33.0f, ImVec2(0, 80));
            }

            // 2. Job System
            if (ImGui::CollapsingHeader("Job System", ImGuiTreeNodeFlags_DefaultOpen))
            {
                auto stats = JobSystem::GetStats();
                
                ImGui::Text("Worker Threads: %d", stats.ThreadCount);
                ImGui::Text("Job Queue: %d", stats.QueueSize);
                
                float fiberUsage = 1.0f - ((float)stats.FreeFibers / (float)stats.TotalFibers);
                ImGui::Text("Fiber Usage: %d / %d", stats.TotalFibers - stats.FreeFibers, stats.TotalFibers);
                ImGui::ProgressBar(fiberUsage, ImVec2(-1, 0), "");
            }

            // 3. Memory
            if (ImGui::CollapsingHeader("Memory", ImGuiTreeNodeFlags_DefaultOpen))
            {
                // GPU
                auto gpuStats = VulkanAllocator::GetStats();
                float usedMB = (float)gpuStats.UsedBytes / (1024.0f * 1024.0f);
                float freeMB = (float)gpuStats.FreeBytes / (1024.0f * 1024.0f);
                float totalMB = usedMB + freeMB;

                ImGui::Text("GPU Memory (VMA)");
                ImGui::Text("Used: %.2f MB", usedMB);
                ImGui::Text("Free: %.2f MB", freeMB);
                ImGui::Text("Allocations: %d", gpuStats.AllocationCount);
                
                if (totalMB > 0.0f)
                    ImGui::ProgressBar(usedMB / totalMB, ImVec2(-1, 0));

                ImGui::Separator();

                // Frame Allocator
                auto renderSystem = Systems::GetSystem<RenderingSystem>();
                if (renderSystem)
                {
                    u64 frameUsed = renderSystem->GetFrameAllocatorUsage();
                    u64 frameTotal = renderSystem->GetFrameAllocatorTotal();
                    float frameUsedKB = (float)frameUsed / 1024.0f;
                    float frameTotalKB = (float)frameTotal / 1024.0f;

                    ImGui::Text("Frame Linear Allocator");
                    ImGui::Text("Used: %.2f KB / %.2f KB", frameUsedKB, frameTotalKB);
                    ImGui::ProgressBar((float)frameUsed / (float)frameTotal, ImVec2(-1, 0));
                }
            }
        }
        ImGui::End();
        ImGui::PopFont();
    }
}