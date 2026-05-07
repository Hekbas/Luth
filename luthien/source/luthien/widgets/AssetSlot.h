#pragma once

#include "luthien/widgets/Properties.h"
#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"

namespace Luth::UI
{
    // Drag-drop-accepting asset slot for inspector property rows. The drop target is filtered
    // against the dragged payload's AssetType; clicking the slot opens the project picker.
    EditState PropertyAsset(const char* label, UUID& assetHandle, AssetType type);
}
