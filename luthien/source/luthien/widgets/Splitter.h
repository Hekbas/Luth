#pragma once

#include <limits>

namespace Luth::UI
{
    // Horizontal splitter strip; drag the strip vertically to resize the bottom
    // region. While active, `*bottomHeight -= mouseDelta.y` and is clamped to
    // [minH, maxH] same-frame so the caller's split layout never overshoots
    // available space (avoids one-frame scrollbar flicker). Returns true on
    // the release frame so callers can persist.
    bool Splitter(const char* id, float* bottomHeight,
                  float minH = 0.0f,
                  float maxH = std::numeric_limits<float>::infinity(),
                  float thickness = 4.0f);
}
