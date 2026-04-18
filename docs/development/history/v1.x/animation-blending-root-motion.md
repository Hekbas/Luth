# Phase 7D — Animation Blending & Root Motion

**Date:** 2026-03-30

---

## Overview

Crossfade transitions between animation clips, layered blending with per-bone masks, and root motion extraction. Built as an additive `AnimationController` component on top of the existing single-clip `Animation` pipeline from Phase 7C — entities without a controller are completely unaffected.

---

## AnimationController Component

**`scene/AnimationController.h`** — New file, lives in `Luth::Component` namespace.

### Core Structs

**`BonePose`** — SQT (Scale-Quaternion-Translation) intermediate representation for blending. Position (`Vec3`), Rotation (`Quat`), Scale (`Vec3`). Blending in SQT space avoids the numerical artifacts of interpolating 4x4 matrices — `glm::mix` for position/scale, `glm::slerp` for rotation, final Mat4 composed once via `ComposeTransform` after all blending is done.

**`BlendLayer`** — One layer of the blend stack. ClipIndex, CurrentTime (seconds), Speed, Weight (0-1), Loop flag, and optional `vector<bool>` BoneMask (empty = all bones affected). Base layer (index 0) always has weight 1.0. Override layers (1+) blend on top.

**`AnimationTransition`** — Active crossfade state. Stores FromClip/ToClip indices, Duration, Elapsed timer, and the from-clip's playback state (FromTime, FromSpeed, FromLoop) so it can continue advancing independently during the blend.

**`AnimationController`** — The component itself. `Layers[]` vector, optional `ActiveTransition`, `CurrentClipIndex`, `ApplyRootMotion` flag, `DefaultTransitionDuration`, and runtime `RootMotionDelta` (written by evaluation job, read by main thread).

### API

- **`Play(clipIndex, transitionDuration)`**: Initiates a crossfade from the current clip to the target. Captures current base layer state into `ActiveTransition.From*` fields, resets base layer to play the new clip from time 0. No-op if `clipIndex == CurrentClipIndex`. Duration defaults to `DefaultTransitionDuration` when passed as -1.
- **`SetLayerClip(layer, clip, weight)`**: Configures an override layer. Auto-resizes `Layers` vector.
- **`SetBoneMask(layer, mask)`**: Sets per-bone enable flags for a layer. Auto-resizes.

---

## Blended Evaluation

### Time Advancement (Main Thread)

Two separate loops in `AnimationSystem::Update()`:

1. **Single-clip entities** (no `AnimationController`): Unchanged from 7C — advance `Animation.CurrentTime` directly.
2. **Controller entities**: Skip the single-clip loop. Instead:
   - Advance each `BlendLayer.CurrentTime` with its own speed/loop settings
   - If `ActiveTransition` exists: advance `Elapsed += dt`, advance `FromTime += dt * FromSpeed`
   - When `Elapsed >= Duration`: clear transition (`reset()`)
   - Reset `RootMotionDelta` to zero
   - Sync `Animation.CurrentTime = Layers[0].CurrentTime` and `Animation.AnimationIndex = CurrentClipIndex` for event detection compatibility (events still fire via the existing 7C mechanism)

### Job Branch

At the top of `EvaluateAnimJob`, after loading the model and skeleton, a branch dispatches to the blended path:

```cpp
if (registry.any_of<AnimationController>(entity)) {
    EvaluateBlended(registry, entity, model, skeleton, anim);
    return;
}
// existing single-clip path unchanged
```

### EvaluateBlended Flow

1. **Sample base pose**:
   - If `ActiveTransition` is active: sample "from" clip at `FromTime` and "to" clip at `Layers[0].CurrentTime`, blend with `alpha = Elapsed / Duration`
   - Otherwise: sample base layer clip directly
   - Fallback to bind pose (decomposed from `LocalBindPose`) if no valid clip

2. **Layered override** (layers 1+):
   - For each layer with `Weight > 0` and valid clip
   - Sample layer clip into `layerPoses`
   - `BlendPoses(basePoses, layerPoses, weight, boneMask)` — masked-out bones are left untouched

3. **Root motion extraction** (if `ApplyRootMotion && clip.HasRootMotion`):
   - Find root bone track (index 0) position channel
   - Compute position delta between previous and current time
   - Loop wrap handling: two-segment delta (`endPos - prevPos` + `currPos - startPos`)
   - Write XZ components to `ctrl.RootMotionDelta` (Y stays on bone for vertical bob)
   - Zero root bone XZ translation in `basePoses[0].Position` to prevent double-movement

4. **Convert + propagate**: `PosesToLocalTransforms()` then `PropagateAndUpload()` (shared tail)

### SQT Helpers

- **`SampleClipSQT(clip, skeleton, timeSeconds, outPoses)`**: Initializes all bones from bind pose by decomposing `LocalBindPose`, then overrides with tracks using the existing `SamplePosition/SampleRotation/SampleScale` methods. Reuses the binary/linear keyframe search from 7C.

- **`BlendPoses(a, b, alpha, result, boneMask)`**: Per-bone `glm::mix` for position/scale, `glm::slerp` for rotation. When `boneMask` is non-empty, bones where `boneMask[i] == false` copy from `a` unchanged.

- **`PosesToLocalTransforms(poses, outLocal)`**: Composes each `BonePose` into a Mat4 via `ComposeTransform(pos, rot, scale)`.

### PropagateAndUpload (Refactor)

Extracted from the single-clip `EvaluateAnimJob` into a shared static method. Contains:
- Hierarchy propagation (single forward pass, topological order)
- `GlobalBoneTransforms` persistence on `Animation` component
- Animated AABB computation (8-corner transform through all bone globals, then world-space transform)
- Skin matrix computation (`global[i] * inverseBindPose[i]`)
- SSBO upload via `BoneMatrixBuffer::UploadBones()`

Both the single-clip path and the blended path call this shared tail, eliminating the previous code duplication.

---

## Root Motion Application

Main thread, after event firing and before bone attachment processing:

```
For each entity with AnimationController + Transform where ApplyRootMotion:
    if length2(RootMotionDelta) < epsilon: skip
    worldDelta = quat(radians(transform.Rotation)) * RootMotionDelta
    transform.Position += worldDelta
    transform.IsDirty = true
```

Delta is computed in model space by the job (safe — one job per entity, no contention). Rotated by entity's current orientation on the main thread. Only XZ components are extracted; Y stays on the bone to preserve vertical motion (breathing, bob).

---

## Serialization

**`SceneSerializer.cpp`** — Save/load `AnimationController`:

Persisted: `currentClipIndex`, `applyRootMotion`, `defaultTransitionDuration`, and per-layer `clipIndex`, `speed`, `weight`, `loop`, `boneMask`.

Bone masks use a sparse format: only enabled bone indices are serialized as a JSON array (e.g., `[0, 1, 5, 12]`). On load, the max index determines vector size, and flagged indices are set to `true`.

Runtime state (`ActiveTransition`, `RootMotionDelta`, layer `CurrentTime`) is not persisted — it resets on scene load.

---

## Editor Inspector

**`InspectorPanel.cpp`** — `DrawComponent<AnimationController>`:

- **Current Clip**: Combo selector populated from `Model::GetAnimationClips()`. Changing triggers `Play()` for crossfade.
- **Root Motion**: Checkbox toggling `ApplyRootMotion`.
- **Transition Duration**: Slider 0-2 seconds for `DefaultTransitionDuration`.
- **Layers**: Per-layer TreeNode with:
  - Clip combo, Speed slider (0-5), Loop checkbox
  - Weight slider (0-1) for override layers (not shown for base layer)
  - Bone Mask tree (override layers only): per-bone checkbox list from skeleton, with All/None/Clear buttons
  - Remove Layer button (not for base layer)
- **Add Layer**: Button appends a new `BlendLayer` with `ClipIndex = 0`

**Add Component menu**: "Animation Controller" entry appears only when the entity has `Animation` but not `AnimationController`. Initializes base layer from current `Animation` state (clip index, speed, loop).

---

## Design Decisions

1. **SQT vs Mat4 blending**: SQT avoids matrix interpolation artifacts (shearing, scale corruption). Cost: one `DecomposeTransform` per bone during `SampleClipSQT` bind-pose init, one `ComposeTransform` per bone at the end. Negligible at <=256 bones.

2. **Both crossfade clips advance**: During a transition, the "from" clip continues playing rather than freezing. This produces natural blends for locomotion (walk-to-run) where both clips have cyclical motion.

3. **Additive overlay pattern**: `AnimationController` adds to `Animation` rather than replacing it. `Animation` remains the owner of `ModelUUID`, `BoneBufferOffset`, `GlobalBoneTransforms`, `AnimatedAABB`, and `OnAnimEvent`. This preserves backward compatibility and avoids migrating existing entity data.

4. **Root motion XZ only**: Y-axis motion stays on the bone to preserve vertical animation (breathing, crouch bob). XZ delta drives horizontal entity translation. This is the standard approach for ground-based locomotion.

5. **Root bone = index 0**: Guaranteed by 7A's topological BFS extraction from the scene root. All root motion code relies on this invariant.

6. **RootMotionDelta on component**: Written by the evaluation job (one job per entity = no contention), read by the main thread after `WaitForCounter`. Same pattern as `GlobalBoneTransforms` and `AnimatedAABB`.

---

## Files Modified/Created

| File | Action | Description |
|------|--------|-------------|
| `scene/AnimationController.h` | CREATE | BonePose, BlendLayer, AnimationTransition, AnimationController |
| `scene/Components.h` | MODIFY | Added `#include "AnimationController.h"` |
| `scene/systems/AnimationSystem.h` | MODIFY | New method declarations (SampleClipSQT, BlendPoses, PosesToLocalTransforms, PropagateAndUpload, EvaluateBlended), forward decl for Component::Animation |
| `scene/systems/AnimationSystem.cpp` | MODIFY | Controller time advancement, EvaluateBlended path, SQT helpers, root motion extraction/application, PropagateAndUpload refactor |
| `scene/SceneSerializer.cpp` | MODIFY | AnimationController serialization (save + load with sparse bone masks) |
| `editor/panels/InspectorPanel.cpp` | MODIFY | AnimationController inspector UI, Add Component menu entry |
| `docs/development/arch/animation-system.md` | MODIFY | Full blending & root motion architecture section |
| `docs/development/ROADMAP.md` | MODIFY | 7D marked complete |
| Project context | MODIFY | Current Progress updated |
