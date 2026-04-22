#pragma once

namespace Luth::ComponentDrawers
{
    // Per-component register functions — each defined in its own .cpp.
    void RegisterPointLight();

    // Umbrella — calls each Register* in canonical order. Called once from
    // Editor::InitPanels.
    void RegisterComponentDrawers();
}
