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

        // Auto-save debounce state
        float m_SaveTimer    = 0.0f;
        bool  m_PendingSave  = false;
        UUID  m_PendingHandle;

        // Undo snapshot: captured when editing begins
        nlohmann::json m_UndoSnapshot;
        bool m_HasUndoSnapshot = false;

        static constexpr float kAutoSaveDelay = 0.5f; // seconds after last edit
    };
}
