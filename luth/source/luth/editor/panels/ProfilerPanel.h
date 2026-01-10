#pragma once

#include "luth/editor/Editor.h"
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
        std::vector<float> m_FrameTimeHistory;
        float m_UpdateTimer = 0.0f;
        
        // Cached stats for display
        float m_FPS = 0.0f;
        float m_FrameTime = 0.0f;
    };
}