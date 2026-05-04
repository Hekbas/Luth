#pragma once

namespace Luth::UI
{
    // Horizontal splitter strip; drag the strip vertically to resize the bottom
    // region. Updates `*bottomHeight` based on mouse delta while active. Caller
    // owns the height value (clamping + persistence). Returns true once on the
    // release frame so callers can persist (e.g., write to EditorSettings).
    bool Splitter(const char* id, float* bottomHeight, float thickness = 4.0f);
}
