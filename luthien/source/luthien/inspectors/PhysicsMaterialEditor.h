#pragma once

#include "luth/core/UUID.h"
#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    class PhysicsMaterial;

    // Inspector pane for PhysicsMaterial assets. Three sliders (friction / restitution / density)
    // with a debounced auto-save: edits don't hit disk on every frame; m_SaveTimer drains and
    // triggers a single Save when the user pauses interaction.
    class PhysicsMaterialEditor
    {
    public:
        void Draw(PhysicsMaterial& material);

    private:
        void Save(PhysicsMaterial& material);

        UUID  m_LastUUID;
        f32   m_SaveTimer   = 0.0f;
        bool  m_PendingSave = false;

        static constexpr f32 kAutoSaveDelay = 0.5f;
    };
}
