#pragma once

#include "luth/core/UUID.h"

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

        static constexpr float kAutoSaveDelay = 0.5f; // seconds after last edit
    };
}
