#include "lepch.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"

// Canonical drawer registration order — matches the inspector layout today.
// Do NOT alphabetize; per-component order is user-visible.
// Drawers will be wired in sub-tasks B and C.

namespace Luth::ComponentDrawers
{
    void RegisterComponentDrawers()
    {
        RegisterPointLight();
    }
}
