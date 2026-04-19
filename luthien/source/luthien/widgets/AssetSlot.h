#pragma once

#include "luth/core/UUID.h"
#include "luth/resources/Asset.h"

namespace Luth::UI
{
    bool PropertyAsset(const char* label, UUID& assetHandle, AssetType type);
}
