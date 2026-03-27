#pragma once

#include "luth/scene/System.h"
#include "luth/jobs/JobSystem.h"
#include "luth/renderer/AnimationClip.h"

namespace Luth
{
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

        // Track allocated entities to detect removals
        std::vector<entt::entity> m_PreviousEntities;
        entt::connection m_DestroyConnection;
    };
}
