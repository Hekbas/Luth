#pragma once

#include "luth/core/UUID.h"
#include "luth/core/types/LuthMath.h"

#include <optional>
#include <vector>

namespace Luth::Component
{
    struct BonePose {
        Vec3 Position = Vec3(0.0f);
        Quat Rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f);
        Vec3 Scale    = Vec3(1.0f);
    };

    struct BlendLayer {
        UUID ClipUUID;            // First-class clip handle (rig-agnostic)
        f32  CurrentTime = 0.0f;  // seconds
        f32  Speed       = 1.0f;
        f32  Weight      = 1.0f;
        bool Loop        = true;
        std::vector<bool> BoneMask;  // empty = all bones affected
    };

    struct AnimationTransition {
        UUID FromClipUUID;
        UUID ToClipUUID;
        f32  Duration  = 0.2f;
        f32  Elapsed   = 0.0f;
        f32  FromTime  = 0.0f;   // "from" clip playback time (keeps advancing)
        f32  FromSpeed = 1.0f;
        bool FromLoop  = true;
    };

    struct AnimationController {
        std::vector<BlendLayer> Layers;  // [0] = base layer
        std::optional<AnimationTransition> ActiveTransition;
        UUID CurrentClipUUID;
        bool ApplyRootMotion  = false;
        f32  DefaultTransitionDuration = 0.2f;

        // Runtime (not serialized) — written by job, read by main thread
        Vec3 RootMotionDelta = Vec3(0.0f);

        void Play(UUID clipUUID, f32 transitionDuration = -1.0f)
        {
            if (clipUUID == CurrentClipUUID) return;
            if (Layers.empty()) Layers.resize(1);

            AnimationTransition t;
            t.FromClipUUID = CurrentClipUUID;
            t.ToClipUUID   = clipUUID;
            t.Duration     = (transitionDuration >= 0.0f) ? transitionDuration : DefaultTransitionDuration;
            t.Elapsed      = 0.0f;
            t.FromTime     = Layers[0].CurrentTime;
            t.FromSpeed    = Layers[0].Speed;
            t.FromLoop     = Layers[0].Loop;
            ActiveTransition = t;

            CurrentClipUUID = clipUUID;
            Layers[0].ClipUUID = clipUUID;
            Layers[0].CurrentTime = 0.0f;
        }

        void SetLayerClip(u32 layer, UUID clip, f32 weight = 1.0f)
        {
            if (layer >= (u32)Layers.size())
                Layers.resize(layer + 1);
            Layers[layer].ClipUUID = clip;
            Layers[layer].Weight = weight;
        }

        void SetBoneMask(u32 layer, const std::vector<bool>& mask)
        {
            if (layer >= (u32)Layers.size())
                Layers.resize(layer + 1);
            Layers[layer].BoneMask = mask;
        }
    };
}
