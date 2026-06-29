#pragma once

#include <imgui.h>

#include <functional>

namespace Luth::UI
{
    // Standard inspector header: thumbnail-on-left, content callback on right.
    // Total height = thumbSize + 2 * padding. `fallbackIcon` (an ICON_* glyph)
    // is centered in the thumbnail box when `thumb` is null. `rightContent` is
    // a free-form callback that runs inside a BeginGroup right of the thumb.
    void InspectorHeader(ImTextureID thumb, const char* fallbackIcon,
                         float thumbSize, std::function<void()> rightContent);
}
