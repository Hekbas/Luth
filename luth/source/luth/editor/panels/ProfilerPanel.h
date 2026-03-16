#pragma once

#include "luth/editor/Editor.h"
#include "luth/memory/MemoryTracker.h"
#include <vector>

namespace Luth
{
    class ProfilerPanel : public Panel
    {
    public:
        ProfilerPanel();
        void OnInit() override;
        void OnRender() override;

    private:
        // Helper to format bytes as B/KB/MB
        static const char* FormatBytes(i64 bytes, char* buf, size_t bufSize);

        std::vector<float> m_FrameTimeHistory;
        float m_UpdateTimer = 0.0f;

        // Cached stats for display
        float m_FPS = 0.0f;
        float m_FrameTime = 0.0f;

        // Memory tracking
        Memory::MemoryTracker::Snapshot m_MemSnapshot{};
        std::vector<float> m_MemoryHistory;  // Total MB over time
    };
}
