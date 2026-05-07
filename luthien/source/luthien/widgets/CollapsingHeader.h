#pragma once

#include <functional>

namespace Luth::UI
{
    // ImGui collapsing-header wrapper that pairs a context menu with the header itself. Used by
    // component drawers; the contextMenu lambda runs on right-click and may detach the component
    // synchronously, so callers must re-guard HasComponent<T>() after Begin returns.
    bool BeginCollapsingHeader(const char* label, bool defaultOpen = false, const std::function<void()>& contextMenu = nullptr);
    void EndCollapsingHeader();
}
