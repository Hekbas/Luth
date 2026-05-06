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

        // Re-hydrate a cached thumbnail from disk (no GPU bake). IOThread reads
        // the PNG → worker decodes → posts as a TextureCompletion identical in
        // shape to the bake path, so Drain installs the descriptor uniformly.
        void DispatchLoadFromDisk(UUID asset, AssetType type);
    }
}
