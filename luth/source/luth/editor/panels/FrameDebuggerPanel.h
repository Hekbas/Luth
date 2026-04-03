#pragma once

#include "luth/editor/Editor.h"
#include "luth/scene/systems/RenderingSystem.h"

#include <memory>

namespace Luth
{
    class FrameDebuggerPanel : public Panel
    {
    public:
        void OnInit() override;
        void OnRender() override;

    private:
        // Live mode (Inactive state) — pass-level view
        void DrawLiveView(const RG::RenderGraphSnapshot& snapshot);
        void DrawLiveControlBar(const RG::RenderGraphSnapshot& snapshot, int nonCulledCount);
        void DrawLivePassTree(const RG::RenderGraphSnapshot& snapshot);
        void DrawLivePassDetails(const RG::RenderGraphSnapshot& snapshot);

        // Capture mode (Frozen state) — draw-call-level view
        void DrawCaptureView(const RG::CapturedFrame& capture);
        void DrawCaptureControlBar(const RG::CapturedFrame& capture);
        void DrawCapturePassTree(const RG::CapturedFrame& capture);
        void DrawCaptureDrawCallDetails(const RG::CapturedFrame& capture);

        std::shared_ptr<RenderingSystem> m_RS;

        // Live mode state
        int  m_SelectedPassIndex     = -1;
        int  m_SelectedResourceIndex = -1;
        int  m_EventSliderValue      = -1;

        // Capture mode state
        int  m_DrawCallSlider    = -1;   // -1 = show all, 0..N-1 = specific draw
        int  m_SelectedDrawIndex = -1;   // which draw call is selected in tree
    };
}
