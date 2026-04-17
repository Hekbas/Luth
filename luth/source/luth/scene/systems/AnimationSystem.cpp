#include "luthpch.h"
#include "luth/scene/systems/AnimationSystem.h"
#include "luth/scene/Scene.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/Components.h"
#include "luth/core/Time.h"
#include "luth/renderer/BoneMatrixBuffer.h"
#include "luth/renderer/Model.h"
#include "luth/resources/AssetManager.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

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

        // Phase 14 — Frame Debugger requires the scene to be visually frozen
        // so per-draw replay (Phase 14E) sees the same bone matrices the live
        // capture wrote into BoneMatrixBuffer. Without this guard, Animation
        // would tick between consecutive replay clicks and each draw would
        // sample a different pose. Mirrors Unity Frame Debugger's pause-while-
        // inspecting behavior. Future: extend to a scene-level pause flag once
        // PhysicsSystem / Audio / scripted systems land.
        if (auto rs = SystemRegistry::GetSystem<RenderingSystem>())
        {
            if (rs->GetDebuggerState() == DebuggerState::Frozen) return;
        }

        auto& registry = scene->Registry();

        // Collect all entities with Animation + WorldTransform (MeshRenderer not required —
        // parent entity owns Animation, children own MeshRenderer)
        auto view = registry.view<Animation, WorldTransform>();
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

        // Advance time (capture previous for event detection)
        f32 dt = Time::DeltaTime();
        for (auto entity : entities)
        {
            auto& anim = registry.get<Animation>(entity);

            // Entities with AnimationController have their own time advancement below
            if (registry.any_of<AnimationController>(entity))
                continue;

            if (!anim.Playing) continue;

            auto model = AssetManager::GetAsset<Model>(anim.ModelUUID);
            if (!model) continue;

            const AnimationClip* clip = model->GetAnimationClip(anim.AnimationIndex);
            if (!clip) continue;

            f32 duration = clip->GetDurationSeconds();
            anim.PreviousTime = anim.CurrentTime;
            anim.CurrentTime += dt * anim.Speed;

            if (anim.LoopMode == AnimationLoopMode::One)
            {
                if (duration > 0.0f)
                    anim.CurrentTime = std::fmod(anim.CurrentTime, duration);
            }
            else if (anim.LoopMode == AnimationLoopMode::All)
            {
                if (anim.CurrentTime >= duration)
                {
                    anim.AnimationIndex = (anim.AnimationIndex + 1) % model->GetAnimationClips().size();
                    anim.CurrentTime = 0.0f;
                }
            }
            else // Off
            {
                if (anim.CurrentTime >= duration)
                {
                    anim.CurrentTime = duration;
                    anim.Playing = false;
                }
            }
        }

        // Advance time for entities with AnimationController
        for (auto entity : entities)
        {
            if (!registry.any_of<AnimationController>(entity))
                continue;

            auto& anim = registry.get<Animation>(entity);
            auto& ctrl = registry.get<AnimationController>(entity);

            if (!anim.Playing) continue;

            auto model = AssetManager::GetAsset<Model>(anim.ModelUUID);
            if (!model) continue;

            const auto& clips = model->GetAnimationClips();

            // Helper to advance a layer's time
            auto advanceLayerTime = [&](BlendLayer& layer) {
                if (layer.ClipIndex < 0 || (u32)layer.ClipIndex >= clips.size()) return;
                f32 dur = clips[layer.ClipIndex].GetDurationSeconds();
                layer.CurrentTime += dt * layer.Speed;
                if (layer.Loop)
                {
                    if (dur > 0.0f)
                        layer.CurrentTime = std::fmod(layer.CurrentTime, dur);
                }
                else
                {
                    if (layer.CurrentTime >= dur)
                        layer.CurrentTime = dur;
                }
            };

            // Advance each blend layer
            for (auto& layer : ctrl.Layers)
                advanceLayerTime(layer);

            // Advance active transition (from-clip time)
            if (ctrl.ActiveTransition)
            {
                auto& t = *ctrl.ActiveTransition;
                t.Elapsed += dt;

                // Advance from-clip time
                if (t.FromClip >= 0 && (u32)t.FromClip < clips.size())
                {
                    f32 fromDur = clips[t.FromClip].GetDurationSeconds();
                    t.FromTime += dt * t.FromSpeed;
                    if (t.FromLoop && fromDur > 0.0f)
                        t.FromTime = std::fmod(t.FromTime, fromDur);
                    else if (t.FromTime >= fromDur)
                        t.FromTime = fromDur;
                }

                // Complete transition
                if (t.Elapsed >= t.Duration)
                    ctrl.ActiveTransition.reset();
            }

            // Reset root motion delta
            ctrl.RootMotionDelta = Vec3(0.0f);

            // Sync Animation component for event detection
            anim.PreviousTime = anim.CurrentTime;
            if (!ctrl.Layers.empty())
                anim.CurrentTime = ctrl.Layers[0].CurrentTime;
            anim.AnimationIndex = ctrl.CurrentClipIndex;
        }

        // Dispatch evaluation jobs (one entity per job)
        EvalJobData jobData;
        jobData.registry = &registry;
        jobData.entities = &entities;
        jobData.totalCount = (u32)entities.size();

        JobSystem::Counter counter;
        JobSystem::Dispatch(jobData.totalCount, 1, EvaluateAnimJob, &jobData, &counter);
        JobSystem::WaitForCounter(&counter);

        // Fire animation events (main thread — safe to modify ECS)
        for (auto entity : entities)
        {
            auto& anim = registry.get<Animation>(entity);
            if (!anim.OnAnimEvent) continue;

            auto model = AssetManager::GetAsset<Model>(anim.ModelUUID);
            if (!model) continue;
            const AnimationClip* clip = model->GetAnimationClip(anim.AnimationIndex);
            if (!clip || clip->Events.empty()) continue;

            f32 tps = (clip->TicksPerSecond > 0.0f) ? clip->TicksPerSecond : 25.0f;
            f32 prev = anim.PreviousTime;
            f32 curr = anim.CurrentTime;

            for (const auto& evt : clip->Events)
            {
                f32 evtSec = evt.Time / tps;
                bool fired = (prev <= curr)
                    ? (evtSec > prev && evtSec <= curr)        // normal forward
                    : (evtSec > prev || evtSec <= curr);       // loop wrap-around
                if (fired)
                    anim.OnAnimEvent(entity, evt.Name);
            }
        }

        // Apply root motion deltas (main thread, after all evaluation)
        for (auto entity : entities)
        {
            if (!registry.any_of<AnimationController>(entity)) continue;
            if (!registry.any_of<Transform>(entity)) continue;

            auto& ctrl = registry.get<AnimationController>(entity);
            if (!ctrl.ApplyRootMotion) continue;
            if (glm::length2(ctrl.RootMotionDelta) < 1e-10f) continue;

            auto& transform = registry.get<Transform>(entity);
            Quat entityRot = glm::quat(glm::radians(transform.Rotation));
            Vec3 worldDelta = entityRot * ctrl.RootMotionDelta;
            transform.Position += worldDelta;
            transform.IsDirty = true;
        }

        // Process bone attachments (main thread, after all evaluation)
        auto attachView = registry.view<BoneAttachment, Transform, WorldTransform>();
        for (auto [entity, attachment, transform, world] : attachView.each())
        {
            if (!attachment.TargetEntity) continue;
            entt::entity targetHandle = (entt::entity)attachment.TargetEntity;
            if (!registry.valid(targetHandle)) continue;
            if (!registry.any_of<Animation>(targetHandle)) continue;
            if (!registry.any_of<WorldTransform>(targetHandle)) continue;

            auto& targetAnim = registry.get<Animation>(targetHandle);
            auto& targetWorld = registry.get<WorldTransform>(targetHandle);

            // Lazy resolve BoneName -> BoneIndex
            if (attachment.BoneIndex == -1 && !attachment.BoneName.empty())
            {
                auto model = AssetManager::GetAsset<Model>(targetAnim.ModelUUID);
                if (model)
                    attachment.BoneIndex = model->GetSkeleton().FindBone(attachment.BoneName);
            }

            if (attachment.BoneIndex < 0 ||
                (u32)attachment.BoneIndex >= (u32)targetAnim.GlobalBoneTransforms.size())
                continue;

            // Bone world transform
            Mat4 boneWorld = targetWorld.Matrix * targetAnim.GlobalBoneTransforms[attachment.BoneIndex];

            // Apply local offset
            Mat4 offset = ComposeTransform(
                attachment.LocalOffset,
                glm::quat(glm::radians(attachment.LocalRotation)),
                Vec3(1.0f));
            Mat4 finalMatrix = boneWorld * offset;

            // Decompose back to Transform
            Vec3 pos, scl;
            Quat rot;
            DecomposeTransform(finalMatrix, pos, rot, scl);
            transform.Position = pos;
            transform.Rotation = glm::degrees(glm::eulerAngles(rot));
            transform.Scale = scl;
            transform.IsDirty = false;

            // Write directly to WorldTransform to avoid one-frame lag
            // (TransformSystem already ran this frame)
            world.Matrix = finalMatrix;
        }
    }

    void AnimationSystem::EvaluateAnimJob(JobSystem::JobArgs args)
    {
        LH_PROFILE_FUNCTION();

        auto* data = static_cast<EvalJobData*>(args.data);
        if (args.jobIndex >= data->totalCount) return;

        entt::entity entity = (*data->entities)[args.jobIndex];
        auto& registry = *data->registry;

        auto& anim = registry.get<Animation>(entity);

        if (!anim.BufferAllocated)
        {
            LH_CORE_WARN("AnimationSystem: Entity has Animation but BufferAllocated=false");
            return;
        }

        auto model = AssetManager::GetAsset<Model>(anim.ModelUUID);
        if (!model)
        {
            LH_CORE_WARN("AnimationSystem: Model asset not loaded for UUID {}", anim.ModelUUID.ToString());
            return;
        }

        const Skeleton& skeleton = model->GetSkeleton();
        if (skeleton.IsEmpty())
        {
            LH_CORE_WARN("AnimationSystem: Skeleton is empty for model '{}'", model->GetName());
            Mat4 identity(1.0f);
            BoneMatrixBuffer::UploadBones(anim.BoneBufferOffset, &identity, 1);
            return;
        }

        // Branch: blended path when AnimationController is present
        if (registry.any_of<AnimationController>(entity))
        {
            EvaluateBlended(registry, entity, model, skeleton, anim);
            return;
        }

        // --- Single-clip path (unchanged from 7C) ---
        u32 boneCount = skeleton.BoneCount();
        const AnimationClip* clip = model->GetAnimationClip(anim.AnimationIndex);

        std::vector<Mat4> localTransforms(boneCount, Mat4(1.0f));

        if (clip && !clip->Tracks.empty())
        {
            f32 tps = (clip->TicksPerSecond > 0.0f) ? clip->TicksPerSecond : 25.0f;
            f32 tickTime = anim.CurrentTime * tps;

            for (u32 i = 0; i < boneCount; i++)
                localTransforms[i] = skeleton.Bones[i].LocalBindPose;

            for (const auto& track : clip->Tracks)
            {
                if (track.BoneIndex < 0 || (u32)track.BoneIndex >= boneCount) continue;

                Vec3 pos = SamplePosition(track, tickTime);
                Quat rot = SampleRotation(track, tickTime);
                Vec3 scl = SampleScale(track, tickTime);

                localTransforms[track.BoneIndex] = ComposeTransform(pos, rot, scl);
            }
        }
        else
        {
            for (u32 i = 0; i < boneCount; i++)
                localTransforms[i] = skeleton.Bones[i].LocalBindPose;
        }

        PropagateAndUpload(skeleton, localTransforms, anim, registry, entity, model);
    }

    // --- Shared tail: hierarchy propagation, AABB, skin matrices, SSBO upload ---
    void AnimationSystem::PropagateAndUpload(
        const Skeleton& skeleton,
        const std::vector<Mat4>& localTransforms,
        Animation& anim,
        entt::registry& registry,
        entt::entity entity,
        const std::shared_ptr<Model>& model)
    {
        u32 boneCount = skeleton.BoneCount();
        std::vector<Mat4> globalTransforms(boneCount);
        std::vector<Mat4> skinMatrices(boneCount);

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

        // Compute animated AABB
        u32 aabbMeshIdx = 0;
        if (registry.any_of<MeshRenderer>(entity))
            aabbMeshIdx = registry.get<MeshRenderer>(entity).MeshIndex;
        if (aabbMeshIdx < model->GetMeshesData().size())
        {
            const AABB& bindAABB = model->GetMeshesData()[aabbMeshIdx].BindPoseAABB;
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

    // --- SQT blending helpers (Phase 7D) ---

    void AnimationSystem::SampleClipSQT(
        const AnimationClip& clip,
        const Skeleton& skeleton,
        f32 timeSeconds,
        std::vector<BonePose>& outPoses)
    {
        u32 boneCount = skeleton.BoneCount();
        outPoses.resize(boneCount);

        // Initialize from bind pose (decompose LocalBindPose into SQT)
        for (u32 i = 0; i < boneCount; i++)
            DecomposeTransform(skeleton.Bones[i].LocalBindPose,
                outPoses[i].Position, outPoses[i].Rotation, outPoses[i].Scale);

        if (clip.Tracks.empty()) return;

        f32 tps = (clip.TicksPerSecond > 0.0f) ? clip.TicksPerSecond : 25.0f;
        f32 tickTime = timeSeconds * tps;

        for (const auto& track : clip.Tracks)
        {
            if (track.BoneIndex < 0 || (u32)track.BoneIndex >= boneCount) continue;

            outPoses[track.BoneIndex].Position = SamplePosition(track, tickTime);
            outPoses[track.BoneIndex].Rotation = SampleRotation(track, tickTime);
            outPoses[track.BoneIndex].Scale    = SampleScale(track, tickTime);
        }
    }

    void AnimationSystem::BlendPoses(
        const std::vector<BonePose>& a,
        const std::vector<BonePose>& b,
        f32 alpha,
        std::vector<BonePose>& result,
        const std::vector<bool>& boneMask)
    {
        u32 count = (u32)std::min(a.size(), b.size());
        result.resize(count);

        for (u32 i = 0; i < count; i++)
        {
            // Skip masked-out bones (leave result unchanged)
            if (!boneMask.empty() && i < boneMask.size() && !boneMask[i])
            {
                if (&result != &a)
                    result[i] = a[i];
                continue;
            }

            result[i].Position = glm::mix(a[i].Position, b[i].Position, alpha);
            result[i].Rotation = glm::slerp(a[i].Rotation, b[i].Rotation, alpha);
            result[i].Scale    = glm::mix(a[i].Scale, b[i].Scale, alpha);
        }
    }

    void AnimationSystem::PosesToLocalTransforms(
        const std::vector<BonePose>& poses,
        std::vector<Mat4>& outLocal)
    {
        outLocal.resize(poses.size());
        for (u32 i = 0; i < (u32)poses.size(); i++)
            outLocal[i] = ComposeTransform(poses[i].Position, poses[i].Rotation, poses[i].Scale);
    }

    void AnimationSystem::EvaluateBlended(
        entt::registry& registry,
        entt::entity entity,
        const std::shared_ptr<Model>& model,
        const Skeleton& skeleton,
        Animation& anim)
    {
        auto& ctrl = registry.get<AnimationController>(entity);
        u32 boneCount = skeleton.BoneCount();
        const auto& clips = model->GetAnimationClips();

        if (ctrl.Layers.empty())
        {
            // No layers configured — fall back to bind pose
            std::vector<Mat4> localTransforms(boneCount);
            for (u32 i = 0; i < boneCount; i++)
                localTransforms[i] = skeleton.Bones[i].LocalBindPose;
            PropagateAndUpload(skeleton, localTransforms, anim, registry, entity, model);
            return;
        }

        // --- Step 1: Sample base pose ---
        std::vector<BonePose> basePoses;

        if (ctrl.ActiveTransition)
        {
            // Crossfade: blend from-clip and to-clip
            auto& t = *ctrl.ActiveTransition;
            f32 alpha = glm::clamp(t.Elapsed / t.Duration, 0.0f, 1.0f);

            std::vector<BonePose> fromPoses, toPoses;

            if (t.FromClip >= 0 && (u32)t.FromClip < clips.size())
                SampleClipSQT(clips[t.FromClip], skeleton, t.FromTime, fromPoses);
            else
            {
                fromPoses.resize(boneCount);
                for (u32 i = 0; i < boneCount; i++)
                    DecomposeTransform(skeleton.Bones[i].LocalBindPose,
                        fromPoses[i].Position, fromPoses[i].Rotation, fromPoses[i].Scale);
            }

            if (ctrl.Layers[0].ClipIndex >= 0 && (u32)ctrl.Layers[0].ClipIndex < clips.size())
                SampleClipSQT(clips[ctrl.Layers[0].ClipIndex], skeleton, ctrl.Layers[0].CurrentTime, toPoses);
            else
                toPoses = fromPoses;

            BlendPoses(fromPoses, toPoses, alpha, basePoses, {});
        }
        else
        {
            // No transition — sample base layer directly
            if (ctrl.Layers[0].ClipIndex >= 0 && (u32)ctrl.Layers[0].ClipIndex < clips.size())
                SampleClipSQT(clips[ctrl.Layers[0].ClipIndex], skeleton, ctrl.Layers[0].CurrentTime, basePoses);
            else
            {
                basePoses.resize(boneCount);
                for (u32 i = 0; i < boneCount; i++)
                    DecomposeTransform(skeleton.Bones[i].LocalBindPose,
                        basePoses[i].Position, basePoses[i].Rotation, basePoses[i].Scale);
            }
        }

        // --- Step 2: Layered override (layers 1+) ---
        for (u32 layerIdx = 1; layerIdx < (u32)ctrl.Layers.size(); layerIdx++)
        {
            const auto& layer = ctrl.Layers[layerIdx];
            if (layer.Weight <= 0.0f || layer.ClipIndex < 0 || (u32)layer.ClipIndex >= clips.size())
                continue;

            std::vector<BonePose> layerPoses;
            SampleClipSQT(clips[layer.ClipIndex], skeleton, layer.CurrentTime, layerPoses);
            BlendPoses(basePoses, layerPoses, layer.Weight, basePoses, layer.BoneMask);
        }

        // --- Step 3: Root motion ---
        if (ctrl.ApplyRootMotion && boneCount > 0)
        {
            i32 baseClipIdx = ctrl.Layers[0].ClipIndex;
            if (baseClipIdx >= 0 && (u32)baseClipIdx < clips.size() && clips[baseClipIdx].HasRootMotion)
            {
                const AnimationClip& baseClip = clips[baseClipIdx];
                f32 tps = (baseClip.TicksPerSecond > 0.0f) ? baseClip.TicksPerSecond : 25.0f;

                // Find root bone's position track
                const BoneTrack* rootTrack = nullptr;
                for (const auto& track : baseClip.Tracks)
                {
                    if (track.BoneIndex == 0) { rootTrack = &track; break; }
                }

                if (rootTrack && !rootTrack->Positions.empty())
                {
                    f32 currTime = ctrl.Layers[0].CurrentTime;
                    f32 prevTime = anim.PreviousTime;

                    Vec3 currRootPos = SamplePosition(*rootTrack, currTime * tps);
                    Vec3 prevRootPos = SamplePosition(*rootTrack, prevTime * tps);

                    Vec3 delta;
                    if (currTime < prevTime && ctrl.Layers[0].Loop)
                    {
                        // Loop wrap: two-segment delta
                        Vec3 endPos  = SamplePosition(*rootTrack, baseClip.Duration);
                        Vec3 startPos = SamplePosition(*rootTrack, 0.0f);
                        delta = (endPos - prevRootPos) + (currRootPos - startPos);
                    }
                    else
                    {
                        delta = currRootPos - prevRootPos;
                    }

                    // Write delta (XZ only — Y stays on the bone for vertical movement)
                    ctrl.RootMotionDelta = Vec3(delta.x, 0.0f, delta.z);

                    // Zero out root bone XZ to prevent double-movement
                    // Keep Y so the character still bobs vertically
                    Vec3 bindRootPos;
                    Quat bindRootRot;
                    Vec3 bindRootScl;
                    DecomposeTransform(skeleton.Bones[0].LocalBindPose,
                        bindRootPos, bindRootRot, bindRootScl);
                    basePoses[0].Position.x = bindRootPos.x;
                    basePoses[0].Position.z = bindRootPos.z;
                }
            }
        }

        // --- Step 4: Convert to Mat4 and propagate ---
        std::vector<Mat4> localTransforms;
        PosesToLocalTransforms(basePoses, localTransforms);
        PropagateAndUpload(skeleton, localTransforms, anim, registry, entity, model);
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
