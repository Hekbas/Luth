#pragma once

#include "luth/core/UUID.h"
#include "luthien/widgets/ThumbnailPreviewScene.h"

#include <nlohmann/json.hpp>

namespace Luth
{
    class Material;

    // Inspector pane for Material assets. Snapshots the JSON before edits begin so a single
    // MaterialSnapshotCommand can be pushed onto CommandHistory once editing pauses (debounced
    // by m_SaveTimer). Embedded in InspectorPanel; previews render through ThumbnailPreviewScene.
    class MaterialEditor
    {
    public:
        void Draw(Material& material);

    private:
        void SaveMaterial(Material& material);

        float m_SaveTimer    = 0.0f;
        bool  m_PendingSave  = false;
        UUID  m_PendingHandle;

        // Captured when editing begins so undo can restore the full pre-edit state.
        nlohmann::json m_UndoSnapshot;
        bool m_HasUndoSnapshot = false;

        // Session-transient feature-reveal state: bits mark optional shading features force-shown for the
        // material being edited, so controls can be authored up from a zero weight without the group
        // collapsing. Reset when the edited material changes; never persisted (the weight is authoritative).
        UUID m_RevealHandle;
        u32  m_RevealMask = 0;

        // Per-MaterialEditor orbit state for the pinned-footer 3D preview.
        UI::ThumbnailPreviewScene::OrbitCamera m_OrbitCam;

        static constexpr float kAutoSaveDelay = 0.5f; // seconds idle before autosave fires
    };
}
