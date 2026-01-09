#include "luthpch.h"
#include "luth/resources/Asset.h"
#include "luth/resources/AssetDatabase.h"

namespace Luth
{
    std::string Asset::GetName() const
    {
        if (!Handle.IsValid()) return "Unsaved Asset";
        const auto& meta = AssetDatabase::GetMetadata(Handle);
        if (meta.Path.empty()) return "Unknown Asset";
        return meta.Path.stem().string();
    }
}