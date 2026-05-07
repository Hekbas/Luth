#pragma once

#include "luth/core/UUID.h"
#include "luth/core/types/LuthMath.h"
#include "luth/scene/Entity.h"
#include "luth/scene/components/AnimationController.h"

#include <entt/entt.hpp>
#include <functional>
#include <string>
#include <vector>

namespace Luth::Component
{
    // Per-entity animation playback state plus the bone-buffer slot assignment used by GPU skinning.
    // BoneAttachment lets a child entity follow a specific bone of an animated parent. AnimationSystem
    // drives playback on the game stage; the renderer reads each entity's resolved BoneMatrixBuffer slot.
    enum class AnimationLoopMode { Off = 0, One = 1, All = 2 };

    struct Animation {
        Animation() = default;
        Animation(UUID uuid) : ModelUUID(uuid) {}
        UUID ModelUUID;
        UUID ClipUUID;  // First-class clip handle. Independent of the model's clip ordering.

        // Playback state
        f32 CurrentTime = 0.0f;   // seconds
        f32 Speed = 1.0f;
        bool Playing = true;
        AnimationLoopMode LoopMode = AnimationLoopMode::One;

        // Runtime (not serialized)
        u32 BoneBufferOffset = UINT32_MAX;  // SSBO base index
        bool BufferAllocated = false;
        f32 PreviousTime = 0.0f;            // for event crossing detection
        std::vector<Mat4> GlobalBoneTransforms;  // per-frame, for attachments/AABB
        AABB AnimatedAABB;                       // world-space animated bounds
        std::function<void(entt::entity, const std::string&)> OnAnimEvent;
    };

    struct BoneAttachment {
        Entity TargetEntity;              // Entity with Animation component
        i32 BoneIndex = -1;               // Resolved at runtime from BoneName
        std::string BoneName;             // Serialized; resolved to BoneIndex via Skeleton::FindBone
        Vec3 LocalOffset = Vec3(0.0f);
        Vec3 LocalRotation = Vec3(0.0f);  // Euler degrees offset in bone space
    };
}
