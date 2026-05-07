#pragma once

#include "luth/core/UUID.h"

namespace Luth
{
    // Read-only inspector pane for a Font asset. Renders the glyph table and metrics. Selected
    // from the ResourcePanel; embedded inside InspectorPanel when a Font is the active asset.
    class FontViewer
    {
    public:
        void Draw(const UUID& fontUUID, const fs::path& fontPath);
    };
}
