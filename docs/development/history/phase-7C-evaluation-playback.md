# Phase 7C — Animation Evaluation & Playback

**Date:** 2026-03-28

---

## Overview

Runtime animation evaluation: fiber-parallel keyframe sampling, skeleton hierarchy propagation, per-frame SSBO upload. Plus animated AABB, animation events with crossing detection, and BoneAttachment socket system. Five commits covering incremental feature additions.

---

## AnimationSystem

**`scene/systems/AnimationSystem.h/.cpp`** — Runs between TransformSystem and CameraSystem.

Update loop:
1. **Collect** entities with `<Animation, WorldTransform>`
2. **Detect removals** — free bone blocks for entities no longer in view
3. **Allocate** bone blocks for new entities (`BoneMatrixBuffer::AllocateBlock()`)
4. **Advance time** — `CurrentTime += dt * Speed`, `fmod` if looping, clamp if not. Captures `PreviousTime` for event detection
5. **Dispatch evaluation jobs** — `JobSystem::Dispatch(entityCount, 1, EvaluateAnimJob)`, one entity per fiber
6. **Fire animation events** — main-thread pass after jobs complete
7. **Process bone attachments** — main-thread pass writing bone transforms to attached entities

### Keyframe Sampling

Per-entity job:
- Convert time to ticks: `tickTime = CurrentTime * TicksPerSecond`
- For each bone: use `LocalBindPose` as default, override with animated tracks
- Binary search for surrounding keyframes (`FindKeyIndex` — linear for <32 keys, binary otherwise)
- Interpolate: `glm::mix` for position/scale, `glm::slerp` for rotation
- Compose: `localTransform = T * R * S`

### Hierarchy Propagation

Single forward pass (bones in topological order from 7A's BFS extraction):
```
for i in 0..boneCount:
    global[i] = (parent >= 0) ? global[parent] * local[i] : local[i]
```

### Skin Matrix Upload

```
skin[i] = global[i] * inverseBindPose[i]
BoneMatrixBuffer::UploadBones(offset, skinMatrices, boneCount)
```

Global bone transforms persisted on Animation component for bone attachments and AABB computation.

---

## Animated AABB

**`core/Math.h`** — `AABB` struct with `Expand`, `Center`, `Extents`, `IsValid` helpers.

- Bind-pose AABB computed from vertex positions during mesh import (both static and skinned paths)
- Reconstructed from vertex data on binary deserialization (no format version bump needed)
- Animated AABB computed per-frame: transform bind-pose AABB corners through each bone's global transform, take union, transform to world space
- Stored in `Animation::AnimatedAABB` for future frustum culling

---

## Animation Events

Events: `AnimationEvent { f32 Time; std::string Name; }` stored on `AnimationClip::Events`.

Crossing detection using `PreviousTime` captured before advancing `CurrentTime`:
- **Normal forward** (`prev <= curr`): fire if `evtSec > prev && evtSec <= curr`
- **Loop wrap-around** (`prev > curr`): fire if `evtSec > prev || evtSec <= curr`

Callbacks via `Animation::OnAnimEvent` on main thread (safe ECS modification).

---

## BoneAttachment Component

```
BoneAttachment {
    Entity TargetEntity;       // Entity with Animation
    i32 BoneIndex = -1;        // Resolved at runtime
    std::string BoneName;      // Serialized; resolved via Skeleton::FindBone
    Vec3 LocalOffset;          // Translation offset in bone space
    Vec3 LocalRotation;        // Euler degrees offset in bone space
};
```

Post-evaluation pass (main thread):
1. Lazy resolve `BoneName -> BoneIndex` via `Skeleton::FindBone()` (cached after first resolution)
2. Compute `boneWorld = targetWorldTransform * globalBoneTransforms[boneIndex]`
3. Apply local offset: `finalMatrix = boneWorld * ComposeTransform(offset, rotation, 1)`
4. Write directly to `WorldTransform.Matrix` (TransformSystem already ran this frame)

Serialization uses deferred UUID resolution (same pattern as parent hierarchy links).

---

## Bone Block Ownership

Moved bone block allocation from RenderingSystem to AnimationSystem. Per-entity `BoneBufferOffset` on Animation component — two entities sharing the same model get independent bone blocks and animate independently.

---

## Files Modified

| File | Change |
|------|--------|
| `scene/systems/AnimationSystem.h` | Full system declaration with job data structs |
| `scene/systems/AnimationSystem.cpp` | **NEW** — Complete evaluation pipeline (~420 lines across commits) |
| `scene/Components.h` | Animation component expansion (CurrentTime, Speed, Playing, Loop, BoneBufferOffset, PreviousTime, GlobalBoneTransforms, AnimatedAABB, OnAnimEvent), BoneAttachment component |
| `scene/Systems.cpp` | AnimationSystem registration between Transform and Camera |
| `scene/SceneSerializer.cpp` | Serialize Animation fields + BoneAttachment with deferred UUID resolution |
| `scene/systems/RenderingSystem.cpp` | Removed bone allocation (moved to AnimationSystem) |
| `scene/systems/RenderingSystem.h` | Cleaned up bone-related fields |
| `core/Math.h` | AABB struct |
| `renderer/Model.h` | MeshData::BindPoseAABB field |
| `resources/AssetSerializer.cpp` | AABB reconstruction on deserialization |
| `resources/importers/ModelImporter.cpp` | Bind-pose AABB computation during import |
