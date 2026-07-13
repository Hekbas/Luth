#pragma once

#include <functional>

namespace Luth::UI
{
    // ImGui collapsing-header wrapper that pairs a context menu with the header itself. Used by
    // component drawers; the contextMenu lambda runs on right-click and may detach the component
    // synchronously, so callers must re-guard HasComponent<T>() after Begin returns.
    bool BeginCollapsingHeader(const char* label, bool defaultOpen = false, const std::function<void()>& contextMenu = nullptr);

    // Enable-toggle variant: draws an on/off checkbox after the caret. Clicking the checkbox flips
    // *enabled and syncs the open state to it (check -> open, uncheck -> collapse); clicking elsewhere
    // on the header collapses independently. Feature-gated inspector groups use this so the toggle
    // lives with its section. Return value is still "body open" (call EndCollapsingHeader only then).
    bool BeginCollapsingHeader(const char* label, bool* enabled, bool defaultOpen = false, const std::function<void()>& contextMenu = nullptr);

    void EndCollapsingHeader();
}
