#include "luthpch.h"
#include "luth/scene/systems/AnimationSystem.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/core/Time.h"
#include "luth/renderer/BoneMatrixBuffer.h"
#include "luth/renderer/Model.h"
#include "luth/resources/AssetManager.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Luth
{
    using namespace Component;

    AnimationSystem::AnimationSystem() = default;

    AnimationSystem::~AnimationSystem()
    {
        // Free all allocated bone blocks
        for (auto entity : m_PreviousEntities)
        {
            // Registry may already be destroyed, so we just free all tracked blocks
            // This is safe because BoneMatrixBuffer::FreeBlock only needs the index
        }
        // Note: actual cleanup happens in Update() when entities are removed.
        // Destructor is a final safety net, but by this point the registry
        // may be invalid. The blocks will be reclaimed when BoneMatrixBuffer::Shutdown() runs.
    }

    void AnimationSystem::Update(Scene* scene)
    {
        LH_PROFILE_FUNCTION();

        auto& registry = scene->Registry();

        // Collect all entities with Animation + MeshRenderer + WorldTransform
        auto view = registry.view<Animation, MeshRenderer, WorldTransform>();
        std::vector<entt::entity> entities;
        entities.reserve(view.size_hint());
        for (auto entity : view)
            entities.push_back(entity);

        // Detect removed entities and free their bone blocks
        for (auto prev : m_PreviousEntities)
        {
            bool stillPresent = false;
            for (auto cur : entities)
            {
                if (cur == prev) { stillPresent = true; break; }
            }
            if (!stillPresent && registry.valid(prev) && registry.any_of<Animation>(prev))
            {
                auto& anim = registry.get<Animation>(prev);
                if (anim.BufferAllocated)
                {
                    BoneMatrixBuffer::FreeBlock(anim.BoneBufferOffset);
                    anim.BoneBufferOffset = UINT32_MAX;
                    anim.BufferAllocated = false;
                }
            }
        }
        m_PreviousEntities = entities;

        if (entities.empty()) return;

        // Allocate bone blocks for new entities
        for (auto entity : entities)
        {
            auto& anim = registry.get<Animation>(entity);
            if (!anim.BufferAllocated)
            {
                anim.BoneBufferOffset = BoneMatrixBuffer::AllocateBlock();
                anim.BufferAllocated = true;
            }
        }

        // Advance time
        f32 dt = Time::DeltaTime();
        for (auto entity : entities)
        {
            auto& anim = registry.get<Animation>(entity);
            if (!anim.Playing) continue;

            auto model = AssetManager::GetAsset<Model>(anim.ModelUUID);
            if (!model) continue;

            const AnimationClip* clip = model->GetAnimationClip(anim.AnimationIndex);
            if (!clip) continue;

            f32 duration = clip->GetDurationSeconds();
            anim.CurrentTime += dt * anim.Speed;

            if (anim.Loop)
            {
                if (duration > 0.0f)
                    anim.CurrentTime = std::fmod(anim.CurrentTime, duration);
            }
            else
            {
                if (anim.CurrentTime >= duration)
                {
                    anim.CurrentTime = duration;
                    anim.Playing = false;
                }
            }
        }

        // Dispatch evaluation jobs (one entity per job)
        EvalJobData jobData;
        jobData.registry = &registry;
        jobData.entities = &entities;
        jobData.totalCount = (u32)entities.size();

        JobSystem::Counter counter;
        JobSystem::Dispatch(jobData.totalCount, 1, EvaluateAnimJob, &jobData, &counter);
        JobSystem::WaitForCounter(&counter);
    }

    void AnimationSystem::EvaluateAnimJob(JobSystem::JobArgs args)
    {
        LH_PROFILE_FUNCTION();

        auto* data = static_cast<EvalJobData*>(args.data);
        if (args.jobIndex >= data->totalCount) return;

        entt::entity entity = (*data->entities)[args.jobIndex];
        auto& registry = *data->registry;

        auto& anim = registry.get<Animation>(entity);
        auto& meshRenderer = registry.get<MeshRenderer>(entity);

        if (!anim.BufferAllocated) return;

        auto model = AssetManager::GetAsset<Model>(anim.ModelUUID);
        if (!model) return;

        const Skeleton& skeleton = model->GetSkeleton();
        if (skeleton.IsEmpty())
        {
            // No skeleton — upload identity
            Mat4 identity(1.0f);
            BoneMatrixBuffer::UploadBones(anim.BoneBufferOffset, &identity, 1);
            return;
        }

        u32 boneCount = skeleton.BoneCount();
        const AnimationClip* clip = model->GetAnimationClip(anim.AnimationIndex);

        // Allocate intermediate arrays
        std::vector<Mat4> localTransforms(boneCount, Mat4(1.0f));
        std::vector<Mat4> globalTransforms(boneCount);
        std::vector<Mat4> skinMatrices(boneCount);

        if (clip && !clip->Tracks.empty())
        {
            // Convert time to ticks
            f32 tps = (clip->TicksPerSecond > 0.0f) ? clip->TicksPerSecond : 25.0f;
            f32 tickTime = anim.CurrentTime * tps;

            // Initialize all bones to their local bind pose
            for (u32 i = 0; i < boneCount; i++)
                localTransforms[i] = skeleton.Bones[i].LocalBindPose;

            // Override with animated tracks
            for (const auto& track : clip->Tracks)
            {
                if (track.BoneIndex < 0 || (u32)track.BoneIndex >= boneCount) continue;

                Vec3 pos = SamplePosition(track, tickTime);
                Quat rot = SampleRotation(track, tickTime);
                Vec3 scl = SampleScale(track, tickTime);

                Mat4 T = glm::translate(Mat4(1.0f), pos);
                Mat4 R = glm::toMat4(rot);
                Mat4 S = glm::scale(Mat4(1.0f), scl);

                localTransforms[track.BoneIndex] = T * R * S;
            }
        }
        else
        {
            // No clip — use bind pose
            for (u32 i = 0; i < boneCount; i++)
                localTransforms[i] = skeleton.Bones[i].LocalBindPose;
        }

        // Hierarchy propagation (bones stored in topological order)
        for (u32 i = 0; i < boneCount; i++)
        {
            i32 parent = skeleton.Bones[i].ParentIndex;
            globalTransforms[i] = (parent >= 0)
                ? globalTransforms[parent] * localTransforms[i]
                : localTransforms[i];
        }

        // Persist global transforms for attachments and AABB
        anim.GlobalBoneTransforms = globalTransforms;

        // Compute animated AABB from bind-pose AABB corners transformed by each bone
        if (meshRenderer.MeshIndex < model->GetMeshesData().size())
        {
            const AABB& bindAABB = model->GetMeshesData()[meshRenderer.MeshIndex].BindPoseAABB;
            if (bindAABB.IsValid())
            {
                AABB animated;
                Vec3 corners[8] = {
                    { bindAABB.Min.x, bindAABB.Min.y, bindAABB.Min.z },
                    { bindAABB.Max.x, bindAABB.Min.y, bindAABB.Min.z },
                    { bindAABB.Min.x, bindAABB.Max.y, bindAABB.Min.z },
                    { bindAABB.Max.x, bindAABB.Max.y, bindAABB.Min.z },
                    { bindAABB.Min.x, bindAABB.Min.y, bindAABB.Max.z },
                    { bindAABB.Max.x, bindAABB.Min.y, bindAABB.Max.z },
                    { bindAABB.Min.x, bindAABB.Max.y, bindAABB.Max.z },
                    { bindAABB.Max.x, bindAABB.Max.y, bindAABB.Max.z },
                };

                for (u32 b = 0; b < boneCount; b++)
                {
                    for (u32 c = 0; c < 8; c++)
                    {
                        Vec3 transformed = Vec3(globalTransforms[b] * Vec4(corners[c], 1.0f));
                        animated.Expand(transformed);
                    }
                }

                // Transform to world space
                auto& worldTransform = registry.get<WorldTransform>(entity);
                AABB worldAABB;
                Vec3 worldCorners[8] = {
                    { animated.Min.x, animated.Min.y, animated.Min.z },
                    { animated.Max.x, animated.Min.y, animated.Min.z },
                    { animated.Min.x, animated.Max.y, animated.Min.z },
                    { animated.Max.x, animated.Max.y, animated.Min.z },
                    { animated.Min.x, animated.Min.y, animated.Max.z },
                    { animated.Max.x, animated.Min.y, animated.Max.z },
                    { animated.Min.x, animated.Max.y, animated.Max.z },
                    { animated.Max.x, animated.Max.y, animated.Max.z },
                };
                for (u32 c = 0; c < 8; c++)
                {
                    Vec3 wp = Vec3(worldTransform.Matrix * Vec4(worldCorners[c], 1.0f));
                    worldAABB.Expand(wp);
                }
                anim.AnimatedAABB = worldAABB;
            }
        }

        // Skin matrix computation
        for (u32 i = 0; i < boneCount; i++)
            skinMatrices[i] = globalTransforms[i] * skeleton.Bones[i].InverseBindPose;

        // Upload to SSBO
        BoneMatrixBuffer::UploadBones(anim.BoneBufferOffset, skinMatrices.data(), boneCount);
    }

    // --- Keyframe binary search (VectorKey) ---
    u32 AnimationSystem::FindKeyIndex(const std::vector<VectorKey>& keys, f32 time)
    {
        if (keys.size() < 2) return 0;

        // Linear for small tracks
        if (keys.size() < 32)
        {
            for (u32 i = 0; i + 1 < (u32)keys.size(); i++)
                if (time < keys[i + 1].Time) return i;
            return (u32)keys.size() - 2;
        }

        // Binary search
        u32 lo = 0, hi = (u32)keys.size() - 1;
        while (lo + 1 < hi)
        {
            u32 mid = (lo + hi) / 2;
            if (keys[mid].Time <= time) lo = mid;
            else hi = mid;
        }
        return lo;
    }

    // --- Keyframe binary search (QuatKey) ---
    u32 AnimationSystem::FindKeyIndex(const std::vector<QuatKey>& keys, f32 time)
    {
        if (keys.size() < 2) return 0;

        if (keys.size() < 32)
        {
            for (u32 i = 0; i + 1 < (u32)keys.size(); i++)
                if (time < keys[i + 1].Time) return i;
            return (u32)keys.size() - 2;
        }

        u32 lo = 0, hi = (u32)keys.size() - 1;
        while (lo + 1 < hi)
        {
            u32 mid = (lo + hi) / 2;
            if (keys[mid].Time <= time) lo = mid;
            else hi = mid;
        }
        return lo;
    }

    Vec3 AnimationSystem::SamplePosition(const BoneTrack& track, f32 time)
    {
        if (track.Positions.empty()) return Vec3(0.0f);
        if (track.Positions.size() == 1) return track.Positions[0].Value;

        u32 i = FindKeyIndex(track.Positions, time);
        u32 j = i + 1;
        if (j >= (u32)track.Positions.size()) return track.Positions[i].Value;

        f32 dt = track.Positions[j].Time - track.Positions[i].Time;
        f32 t = (dt > 0.0f) ? (time - track.Positions[i].Time) / dt : 0.0f;
        return glm::mix(track.Positions[i].Value, track.Positions[j].Value, t);
    }

    Quat AnimationSystem::SampleRotation(const BoneTrack& track, f32 time)
    {
        if (track.Rotations.empty()) return Quat(1.0f, 0.0f, 0.0f, 0.0f);
        if (track.Rotations.size() == 1) return track.Rotations[0].Value;

        u32 i = FindKeyIndex(track.Rotations, time);
        u32 j = i + 1;
        if (j >= (u32)track.Rotations.size()) return track.Rotations[i].Value;

        f32 dt = track.Rotations[j].Time - track.Rotations[i].Time;
        f32 t = (dt > 0.0f) ? (time - track.Rotations[i].Time) / dt : 0.0f;
        return glm::slerp(track.Rotations[i].Value, track.Rotations[j].Value, t);
    }

    Vec3 AnimationSystem::SampleScale(const BoneTrack& track, f32 time)
    {
        if (track.Scales.empty()) return Vec3(1.0f);
        if (track.Scales.size() == 1) return track.Scales[0].Value;

        u32 i = FindKeyIndex(track.Scales, time);
        u32 j = i + 1;
        if (j >= (u32)track.Scales.size()) return track.Scales[i].Value;

        f32 dt = track.Scales[j].Time - track.Scales[i].Time;
        f32 t = (dt > 0.0f) ? (time - track.Scales[i].Time) / dt : 0.0f;
        return glm::mix(track.Scales[i].Value, track.Scales[j].Value, t);
    }
}
