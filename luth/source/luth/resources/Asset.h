#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/UUID.h"
#include <string>

namespace Luth
{
    enum class AssetType
    {
        None = 0,
        Texture,
        Model,
        Material,
        Shader,
        Font,
        Scene
    };

    enum class AssetFlag { None = 0, Missing = 1, Invalid = 2, Loading = 4 };

    class Asset
    {
    public:
        virtual ~Asset() = default;
        
        UUID Handle;
        AssetFlag Flags = AssetFlag::None;
        f32 LastAccessedTime = 0.0f;
        
        virtual AssetType GetType() const = 0;
        bool IsValid() const { return !((int)Flags & (int)AssetFlag::Invalid); }
        bool IsLoaded() const { return !((int)Flags & (int)AssetFlag::Loading); }

        std::string GetName() const;
    };
}