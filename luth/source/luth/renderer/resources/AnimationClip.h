#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/resources/Asset.h"

#include <string>
#include <vector>

namespace Luth
{
    // Animation keyframe data. BoneTrack carries per-bone position / rotation / scale keys;
    // AnimationClip is the UUID-addressable asset imported from Assimp's animation channels.
    // AnimationSystem samples it each game-stage tick to produce the current pose.
    struct VectorKey {
        f32  Time;
        Vec3 Value;
    };

    struct QuatKey {
        f32  Time;
        Quat Value;
    };

    struct BoneTrack {
        i32 BoneIndex = -1;
        std::vector<VectorKey> Positions;
        std::vector<QuatKey>   Rotations;
        std::vector<VectorKey> Scales;
    };

    struct AnimationEvent {
        f32 Time;               // Timestamp in ticks
        std::string Name;       // e.g. "FootstepL", "AttackHit"
    };

    // First-class UUID-addressable asset. Inheriting Asset adds a vtable +
    // Handle/Flags/LastAccessedTime; size growth is irrelevant to the
    // existing field-by-field serializer.
    class AnimationClip : public Asset
    {
    public:
        std::string Name;
        f32 Duration        = 0.0f;  // In ticks
        f32 TicksPerSecond  = 0.0f;
        std::vector<BoneTrack> Tracks;
        std::vector<AnimationEvent> Events;
        bool HasRootMotion = false;

        AssetType GetType() const override { return AssetType::Animation; }

        f32 GetDurationSeconds() const
        {
            f32 tps = (TicksPerSecond > 0.0f) ? TicksPerSecond : 25.0f;
            return Duration / tps;
        }
    };
}
