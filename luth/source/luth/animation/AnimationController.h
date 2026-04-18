#pragma once

#include "luth/core/LuthTypes.h"

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
        i32  ClipIndex   = -1;
        f32  CurrentTime = 0.0f;  // seconds
        f32  Speed       = 1.0f;
        f32  Weight      = 1.0f;
        bool Loop        = true;
        std::vector<bool> BoneMask;  // empty = all bones affected
    };

    struct AnimationTransition {
        i32  FromClip  = -1;
        i32  ToClip    = -1;
        f32  Duration  = 0.2f;
        f32  Elapsed   = 0.0f;
        f32  FromTime  = 0.0f;   // "from" clip playback time (keeps advancing)
        f32  FromSpeed = 1.0f;
        bool FromLoop  = true;
    };

    struct AnimationController {
        std::vector<BlendLayer> Layers;  // [0] = base layer
        std::optional<AnimationTransition> ActiveTransition;
        i32  CurrentClipIndex = 0;
        bool ApplyRootMotion  = false;
        f32  DefaultTransitionDuration = 0.2f;

        // Runtime (not serialized) — written by job, read by main thread
        Vec3 RootMotionDelta = Vec3(0.0f);

        void Play(i32 clipIndex, f32 transitionDuration = -1.0f)
        {
            if (clipIndex == CurrentClipIndex) return;
            if (Layers.empty()) Layers.resize(1);

            AnimationTransition t;
            t.FromClip  = CurrentClipIndex;
            t.ToClip    = clipIndex;
            t.Duration  = (transitionDuration >= 0.0f) ? transitionDuration : DefaultTransitionDuration;
            t.Elapsed   = 0.0f;
            t.FromTime  = Layers[0].CurrentTime;
            t.FromSpeed = Layers[0].Speed;
            t.FromLoop  = Layers[0].Loop;
            ActiveTransition = t;

            CurrentClipIndex = clipIndex;
            Layers[0].ClipIndex = clipIndex;
            Layers[0].CurrentTime = 0.0f;
        }

        void SetLayerClip(u32 layer, i32 clip, f32 weight = 1.0f)
        {
            if (layer >= (u32)Layers.size())
                Layers.resize(layer + 1);
            Layers[layer].ClipIndex = clip;
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
