#include "luthpch.h"
#include "AnimationClipImporter.h"
#include "luth/resources/AssetSerializer.h"

namespace Luth
{
    bool AnimationClipImporter::Import(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        // Source already matches the artifact format (ModelImporter writes the
        // cooked binary directly). Re-cook validates the header and keeps the
        // load path uniform with text-source asset types.
        AnimationAssetData data;
        if (!AssetSerializer::DeserializeAnimation(source, data))
        {
            LH_CORE_ERROR("AnimationClipImporter: Failed to read source {0}", source.string());
            return false;
        }
        return AssetSerializer::SerializeAnimation(destination, data);
    }
}
