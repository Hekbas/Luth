#pragma once

namespace Luth::UI
{
    // Hoverable "(?)" glyph that reveals `desc` as a wrapped tooltip. Inline author-facing help for
    // inspector rows/headers; place after a label with ImGui::SameLine(). Text is not copied.
    void HelpMarker(const char* desc);
}
