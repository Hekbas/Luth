#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"

namespace Luth::UI
{
    // Async bake dispatch — worker job posts to ThumbnailCache's completion queue.
    namespace ThumbnailGenerator
    {
        // Fire-and-forget. UnsupportedType / non-Vulkan / no project → no-op
        // (Get's gates already filter most of these — kept here for safety).
        void Dispatch(UUID asset, AssetType type);
    }
}
