#pragma once

#include "luth/scene/System.h"
#include "luth/scene/AnimationController.h"
#include "luth/jobs/JobSystem.h"
#include "luth/renderer/AnimationClip.h"
#include "luth/renderer/Skeleton.h"

#include <memory>

namespace Luth::Component { struct Animation; }

namespace Luth
{
    class Model;

    class AnimationSystem : public System
    {
    public:
        AnimationSystem();
        ~AnimationSystem();
        void Update(Scene* scene) override;

    private:
        struct EvalJobData {
            entt::registry* registry;
            std::vector<entt::entity>* entities;
            u32 totalCount;
        };
        static void EvaluateAnimJob(JobSystem::JobArgs args);

        // Keyframe sampling
        static u32 FindKeyIndex(const std::vector<VectorKey>& keys, f32 time);
        static u32 FindKeyIndex(const std::vector<QuatKey>& keys, f32 time);
        static Vec3 SamplePosition(const BoneTrack& track, f32 time);
        static Quat SampleRotation(const BoneTrack& track, f32 time);
        static Vec3 SampleScale(const BoneTrack& track, f32 time);

        // SQT blending helpers (Phase 7D)
        using BonePose = Component::BonePose;

        static void SampleClipSQT(
            const AnimationClip& clip,
            const Skeleton& skeleton,
            f32 timeSeconds,
            std::vector<BonePose>& outPoses);

        static void BlendPoses(
            const std::vector<BonePose>& a,
            const std::vector<BonePose>& b,
            f32 alpha,
            std::vector<BonePose>& result,
            const std::vector<bool>& boneMask);

        static void PosesToLocalTransforms(
            const std::vector<BonePose>& poses,
            std::vector<Mat4>& outLocal);

        static void PropagateAndUpload(
            const Skeleton& skeleton,
            const std::vector<Mat4>& localTransforms,
            Component::Animation& anim,
            entt::registry& registry,
            entt::entity entity,
            const std::shared_ptr<Model>& model);

        static void EvaluateBlended(
            entt::registry& registry,
            entt::entity entity,
            const std::shared_ptr<Model>& model,
            const Skeleton& skeleton,
            Component::Animation& anim);

        // Track allocated entities to detect removals
        std::vector<entt::entity> m_PreviousEntities;
        entt::connection m_DestroyConnection;
    };
}
