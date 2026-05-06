#pragma once

#include "luth/core/UUID.h"
#include "luthien/widgets/ThumbnailPreviewScene.h"

#include <nlohmann/json.hpp>

namespace Luth
{
    class Material;

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

        // Per-MaterialEditor orbit state for the pinned-footer 3D preview.
        UI::ThumbnailPreviewScene::OrbitCamera m_OrbitCam;

        static constexpr float kAutoSaveDelay = 0.5f; // seconds idle before autosave fires
    };
}
