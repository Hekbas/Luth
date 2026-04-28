#pragma once

#include "luth/resources/AssetImporter.h"
#include "luth/renderer/resources/AnimationClip.h"

namespace Luth
{
    // Source .anim file contains a single cooked clip. Format matches the
    // artifact byte-for-byte; ModelImporter writes these alongside the FBX
    // (see <stem>_Animations/<clipName>.anim) so users can drag-drop them
    // independently of the parent model.
    struct AnimationAssetData : public AssetData
    {
        AnimationClip Clip;
    };

    class AnimationClipImporter : public AssetImporter
    {
    public:
        bool Import(const std::filesystem::path& source, const std::filesystem::path& destination) override;
    };
}
