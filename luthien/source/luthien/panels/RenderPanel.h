#pragma once

#include "luthien/Editor.h"
#include "luth/core/UUID.h"
#include "luth/scene/systems/RenderingSystem.h"

#include <map>
#include <string>
#include <functional>

namespace Luth
{
    // Renderer settings panel: debug attachment toggles, post-process tunables, model preview.
    // Reads and writes RenderingSystem state directly. The settings UI is dominantly ImGui-driven,
    // so the snapshot is intentionally empty — there's nothing useful to gather on a worker fiber.
    struct RenderSettingsSnapshot { /* placeholder; settings UI is fully ImGui-driven */ };

    class RenderPanel : public Panel
    {
    public:
        RenderPanel();
        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

        u32 GetSelectedAttachment() const { return m_SelectedAttachment; }

    private:
        
        RenderingSystem* m_RS = nullptr;
        std::string m_SelectedMode;
        u32 m_SelectedAttachment = 0;

        u32 m_SelectedTab = 0; // 0 for Model Viewer, 1 for Post Processing
    };
}
