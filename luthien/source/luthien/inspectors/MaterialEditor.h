#pragma once

#include "luth/core/UUID.h"

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

        static constexpr float kAutoSaveDelay = 0.5f; // seconds idle before autosave fires
    };
}
