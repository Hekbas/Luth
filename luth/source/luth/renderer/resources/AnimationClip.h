#pragma once

#include "luth/core/types/LuthMath.h"

#include <string>
#include <vector>

namespace Luth
{
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

    struct AnimationClip {
        std::string Name;
        f32 Duration        = 0.0f;  // In ticks
        f32 TicksPerSecond  = 0.0f;
        std::vector<BoneTrack> Tracks;
        std::vector<AnimationEvent> Events;
        bool HasRootMotion = false;

        f32 GetDurationSeconds() const
        {
            f32 tps = (TicksPerSecond > 0.0f) ? TicksPerSecond : 25.0f;
            return Duration / tps;
        }
    };
}
