#pragma once

#include "luthien/Editor.h"
#include "luthien/viewport/ViewportRenderer.h"
#include "luth/renderer/FrameTargets.h"

#include <memory>

namespace Luth
{
    class RenderingSystem;

    // Read-only "what the player sees" viewport. Owns its own FrameTargets so
    // it can resize independently of the Scene panel, and drives a second
    // RS::RenderToView per frame through the first Component::Camera entity
    // found in the active scene. No overlays — no grid, outline, gizmos, or
    // selection outline. Placeholder text when no Camera entity exists.
    class GamePanel : public Panel
    {
    public:
        explicit GamePanel(RenderingSystem* renderingSystem);
        ~GamePanel() override;

        void OnInit() override;
        void OnRender() override;

    private:
        RenderingSystem* m_RenderingSystem = nullptr;
        FrameTargets     m_Targets;
        bool             m_TargetsAllocated = false;
        std::unique_ptr<ViewportRenderer> m_Viewport;
    };
}
