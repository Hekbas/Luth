#pragma once

#include "luthien/Editor.h"
#include "luth/scene/systems/RenderingSystem.h"

namespace Luth
{
    // Renderer settings panel: category-tabbed feature tuning (Lighting / GI-RT / Denoise / Atmosphere /
    // Post FX / Reference / Diagnostics) plus a search filter. Reads/writes RenderingSystem settings live;
    // the snapshot stays empty since the UI is fully ImGui-driven. Editor-visual prefs (grid/outline/env)
    // live in Preferences, not here.
    struct RenderSettingsSnapshot {};

    class RenderPanel : public Panel
    {
    public:
        RenderPanel();
        void OnInit() override;
        void OnGather(EditorSnapshotBuilder& builder) override;
        void OnDraw(const EditorSnapshot& snapshot) override;

    private:
        RenderingSystem* m_RS = nullptr;
        char m_Filter[64] = {};   // section search; non-empty bypasses the category tabs
    };
}
