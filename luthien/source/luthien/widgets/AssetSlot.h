#pragma once

#include "luthien/widgets/Properties.h"
#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"

namespace Luth::UI
{
    EditState PropertyAsset(const char* label, UUID& assetHandle, AssetType type);
}
