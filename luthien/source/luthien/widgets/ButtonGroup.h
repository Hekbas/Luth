#pragma once

#include <functional>

namespace Luth::UI
{
    // Fused row of N text-labeled buttons. Active idx is highlighted with
    // ImGuiCol_ButtonActive; clicking a different idx writes back through
    // selectedIdx and returns true.
    bool SegmentedButton(const char* groupId, const char* const* labels, int count, int* selectedIdx);

    // Row of N square icon-only buttons (FrameHeight × FrameHeight) with 2px
    // spacing. Mirrors the active-state push pattern used by ScenePanel's gizmo
    // toolbar. tooltips may be nullptr or contain nullptr entries to skip.
    bool IconToggleGroup(const char* groupId, const char* const* icons, const char* const* tooltips, int count, int* selectedIdx);

    // Single icon button with persistent active state. Toggles *state on click.
    bool IconToggleButton(const char* label, const char* icon, const char* tooltip, bool* state);

    // Unity-style split-button: icon half (toggles *state) + chevron half
    // (opens a popup running dropdownBody). Returns true on icon-half click.
    // Pass state=nullptr if the icon should not own a toggle state.
    bool SplitToggleButton(const char* groupId, const char* icon, const char* tooltip,
                           bool* state, const std::function<void()>& dropdownBody);
}
