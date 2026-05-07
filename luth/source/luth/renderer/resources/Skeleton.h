#pragma once

#include "luth/core/types/LuthMath.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace Luth
{
    // Bind-pose bone hierarchy for a Model. Bones are stored in topological order (parent before
    // child) so a single forward sweep is enough to compose world matrices. Owned by Model;
    // per-entity playback state lives separately on Component::Animation.
    static constexpr u32 MAX_BONES_PER_VERTEX = 4;
    static constexpr u32 MAX_BONES = 256;

    struct BoneInfo {
        std::string Name;
        i32 ParentIndex = -1;              // -1 = root
        Mat4 InverseBindPose = Mat4(1.0f);
        Mat4 LocalBindPose   = Mat4(1.0f); // aiNode::mTransformation
    };

    struct Skeleton {
        std::vector<BoneInfo> Bones;       // Topological order (parent before child)
        std::unordered_map<std::string, i32> BoneNameToIndex;

        i32 FindBone(const std::string& name) const
        {
            auto it = BoneNameToIndex.find(name);
            return (it != BoneNameToIndex.end()) ? it->second : -1;
        }

        bool IsEmpty() const { return Bones.empty(); }
        u32 BoneCount() const { return static_cast<u32>(Bones.size()); }
    };
}
