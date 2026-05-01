#pragma once

#include "luthien/Editor.h"
#include "luthien/viewport/ViewportRenderer.h"
#include "luth/renderer/FrameTargets.h"

#include <memory>

namespace Luth
{
    class RenderingSystem;

    struct GameViewportSnapshot { /* placeholder; populated by future polish */ };

    // Read-only viewport rendering through the first Component::Camera
    // entity in the active scene. Queues its view each frame for
    // RenderingSystem::Update to record alongside the scene view. No
    // overlays. Owns its own FrameTargets so it resizes independently
    // of the scene panel. Placeholder when no Camera entity exists.
    class GamePanel : public Panel
    {
    public:
        explicit GamePanel(RenderingSystem* renderingSystem);
        ~GamePanel() override;

        void OnInit() override;
        bool UsesNewLifecycle() const override { return true; }
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

    private:
        RenderingSystem* m_RenderingSystem = nullptr;
        FrameTargets     m_Targets;
        bool             m_TargetsAllocated = false;
        std::unique_ptr<ViewportRenderer> m_Viewport;
    };
}
