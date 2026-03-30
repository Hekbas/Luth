# Animation System Architecture

## Overview

Skeletal animation pipeline for the Luth engine. CPU-side keyframe evaluation with GPU skinning via SSBO. Designed around the fiber-based job system and triple-buffered frame pipeline.

## Data Model (Phase 7A)

### Skeleton (`renderer/Skeleton.h`)

```
Skeleton
├── Bones[]           — BoneInfo array in topological order (parent before child)
│   ├── Name          — Bone name (matches Assimp node name)
│   ├── ParentIndex   — Index into Bones[], -1 for root
│   ├── InverseBindPose — mat4, from aiBone::mOffsetMatrix
│   └── LocalBindPose — mat4, from aiNode::mTransformation
└── BoneNameToIndex   — Fast name→index lookup
```

Constants: `MAX_BONES = 256`, `MAX_BONES_PER_VERTEX = 4`

### AnimationClip (`renderer/AnimationClip.h`)

```
AnimationClip
├── Name, Duration (ticks), TicksPerSecond
├── HasRootMotion     — Root bone has translation keyframes
├── Tracks[]          — One BoneTrack per animated bone
│   ├── BoneIndex     — Index into Skeleton::Bones
│   ├── Positions[]   — VectorKey { Time, Vec3 }
│   ├── Rotations[]   — QuatKey { Time, Quat }
│   └── Scales[]      — VectorKey { Time, Vec3 }
└── Events[]          — AnimationEvent { Time, Name }
```

Separate vectors per channel allow independent keyframe counts and efficient binary search.

### Vertex Formats (`renderer/Model.h`)

| Format | Size | Usage |
|--------|------|-------|
| `Vertex` | 52 bytes | Static meshes (pos + normal + uv0 + uv1 + tangent) |
| `SkinnedVertex` | 84 bytes | Skinned meshes (same + ivec4 BoneIDs + vec4 BoneWeights) |

Two separate structs to avoid wasting 32 bytes per vertex on static geometry.

`MeshData::IsSkinned` flag selects which vertex vector is populated and which GPU layout is used.

### Data Flow

```
FBX/glTF source file
  ↓ ModelImporter (Assimp + aiProcess_LimitBoneWeights)
  ├── Load ModelImportSettings from .meta (scale, axis, mesh transform mode)
  ├── ExtractSkeleton() — BFS from root, topological order, mesh-space correction
  ├── ProcessSkinnedMesh() — Bakes mesh node transform into vertices, bone weights normalized
  └── ExtractAnimationClips() — Keyframes copied per channel
  ↓
ModelAssetData { Meshes, Materials, Skeleton, AnimationClips, IsSkinned }
  ↓ AssetSerializer::SerializeModel() — V2 binary format
  ↓
Binary artifact (.luth)
  ↓ AssetSerializer::DeserializeModel() — V1/V2 backward compatible
  ↓
Model::Create() — ProcessMeshData() uploads to GPU with appropriate vertex layout
  ├── Static: Float3+Float3+Float2+Float2+Float3
  └── Skinned: same + Int4 + Float4
```

### Key Design Decisions

- **Axis correction for skinned models**: Applied to skeleton root's LocalBindPose, NOT per-vertex. Prevents double-application when bone transforms propagate through hierarchy.
- **Mesh transform baking**: `ProcessSkinnedMesh()` bakes the accumulated mesh-node transform into vertices so they match the skeleton's coordinate space.
- **FBX export-pose fix**: `ExtractSkeleton()` reconstructs true T-pose from `mOffsetMatrix` (inverse bind pose), bypassing `mTransformation` which stores the timeline frame at export time, not the bind pose.
- **Mesh-space correction**: `ComputeMeshSpaceCorrection()` detects DCC-baked rotations on mesh nodes that `mOffsetMatrix` (mesh-local space) doesn't account for. Applied during skeleton reconstruction to lift bone poses into engine space. Gated by `ModelImportSettings::MeshTransformMode` (Auto/Bake/Identity).
- **Per-asset import settings**: `ModelImportSettings` struct stored in `.meta` files. Controls geometry flags (normals, tangents, optimize), scale factor, axis override, and skinning mesh-transform mode. Editor exposes all settings with Apply+reimport flow.
- **Binary format versioning**: `AssetHeader::Version >= 2` indicates skeleton data present. V1 files load as static meshes.

## GPU Skinning Pipeline (Phase 7B)

### BoneMatrixBuffer (`renderer/BoneMatrixBuffer.h/.cpp`)

Static singleton following the MaterialSystem pattern. Persistently-mapped SSBO for bone matrices.

```
Layout:     128 entity blocks × 256 bones × mat4 (64 bytes) = 2 MB
Descriptor: Set 4, Binding 0, STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT
Memory:     VMA_MEMORY_USAGE_CPU_TO_GPU (host-visible, host-coherent)
```

API: `AllocateBlock() → u32 baseIndex`, `FreeBlock(u32)`, `UploadBones(u32 base, Mat4*, count)`.

Init fills entire buffer with identity matrices — skinned meshes render in bind pose without explicit uploads.

### Descriptor Set Layout (5 sets)

```
Set 0: GlobalUniforms UBO + IBL samplers
Set 1: Bindless textures (16384)
Set 2: MaterialSystem SSBO
Set 3: Light UBO + shadow sampler
Set 4: BoneMatrixBuffer SSBO          ← NEW
```

All geometry/shadow/skybox pipelines use the 5-set layout. Skybox ignores Set 4.

### Push Constants (80 bytes, unchanged size)

```cpp
struct ObjectPushConstants {
    mat4 modelMatrix;   // 64 bytes
    u32  materialIndex; // 4 bytes
    u32  shadeMode;     // 4 bytes
    u32  entityID;      // 4 bytes
    u32  boneOffset;    // 4 bytes — base index into BoneMatrices SSBO (was _pad)
};
```

Static meshes pass `boneOffset = 0` (ignored by static shader).

### Shader Variants

| Shader | Purpose | Inputs |
|--------|---------|--------|
| `pbr.vert` | Static geometry | locations 0-4 (52-byte stride) |
| `pbr_skinned.vert` | Skinned geometry | locations 0-6 (84-byte stride), Set 4 SSBO |
| `shadowDepth.vert` | Static shadow | location 0 (stride override to 52) |
| `shadowDepth_skinned.vert` | Skinned shadow | locations 0-6 (84-byte stride), Set 4 SSBO |

All share `pbr.frag` / `shadowDepth.frag` respectively (fragment shaders unchanged).

### Linear Blend Skinning (LBS)

```glsl
mat4 skinMatrix = mat4(0.0);
for (int i = 0; i < 4; i++) {
    if (a_BoneIDs[i] >= 0)
        skinMatrix += a_BoneWeights[i] * bones[pc.boneOffset + a_BoneIDs[i]];
}
// Fallback for unweighted vertices
if (skinMatrix == mat4(0.0)) skinMatrix = mat4(1.0);

// Apply skinning before model transform
vec4 skinnedPos = skinMatrix * vec4(a_Position, 1.0);
vec3 skinnedNormal = normalize(mat3(skinMatrix) * a_Normal);
```

### Pipeline Selection

Two `PipelineManager` instances (static + skinned) with different vertex layouts captured in their ConfigFactory closures. Per-mesh `MeshData::IsSkinned` flag determines which pipeline to bind during draw.

Shadow pass uses two separate `VKPipeline` objects (`m_ShadowPipeline`, `m_ShadowSkinnedPipeline`).

### Bone Block Allocation

Bone blocks are owned per-entity by the `Animation` component (`BoneBufferOffset`). AnimationSystem allocates blocks on first update via `BoneMatrixBuffer::AllocateBlock()` and frees them when entities are removed. RenderingSystem reads `BoneBufferOffset` from the Animation component during draw command collection — two entities sharing the same model get independent bone blocks and animate independently.

## Runtime Evaluation (Phase 7C)

### AnimationSystem (`scene/systems/AnimationSystem.h/.cpp`)

Runs between TransformSystem and CameraSystem. Full update loop:

1. **Collect** entities with `<Animation, WorldTransform>`
2. **Detect removals** — free bone blocks for entities no longer in the view
3. **Allocate** bone blocks for new entities (`BoneMatrixBuffer::AllocateBlock()`)
4. **Advance time** — single-clip entities: `CurrentTime += dt * Speed`, `fmod` if looping, clamp if not. Controller entities: advance each layer independently, advance transition, sync `Animation.CurrentTime` from base layer (see Phase 7D)
5. **Dispatch evaluation jobs** — `JobSystem::Dispatch(entityCount, 1, EvaluateAnimJob)`, one entity per fiber. Branches to `EvaluateBlended()` if `AnimationController` present, else single-clip path
6. **Fire animation events** — main-thread pass after jobs complete
7. **Apply root motion** — main-thread pass for entities with `AnimationController::ApplyRootMotion` (Phase 7D)
8. **Process bone attachments** — main-thread pass writing bone transforms to attached entities

### Keyframe Sampling

Per-entity job (`EvaluateAnimJob`):

1. Convert time to ticks: `tickTime = CurrentTime * TicksPerSecond`
2. For each bone: use `LocalBindPose` as default, override with animated tracks
3. Binary search for surrounding keyframes (`FindKeyIndex` — linear for <32 keys, binary otherwise)
4. Interpolate: `glm::mix` for position/scale, `glm::slerp` for rotation
5. Compose: `localTransform = T * R * S`

### Hierarchy Propagation

Single forward pass (bones stored in topological order by 7A's BFS extraction):

```
for i in 0..boneCount:
    global[i] = (parent >= 0) ? global[parent] * local[i] : local[i]
```

### Skin Matrix Computation + Upload

```
skin[i] = global[i] * inverseBindPose[i]
BoneMatrixBuffer::UploadBones(offset, skinMatrices, boneCount)
```

Global bone transforms are also persisted on the Animation component for use by bone attachments and animated AABB computation.

### Animated AABB

Computed per-entity during evaluation:

1. Extract 8 corners of the mesh's bind-pose AABB (`MeshData::BindPoseAABB`)
2. Transform each corner by each bone's global transform → bone-space AABB
3. Transform to world space via entity's WorldTransform
4. Stored in `Animation::AnimatedAABB` (ready for future frustum culling)

The `AABB` struct (`core/Math.h`) provides `Expand`, `Center`, `Extents`, `IsValid` helpers. Bind-pose AABBs are computed from vertex positions during mesh import (both static and skinned paths) and reconstructed from vertex data on binary deserialization.

### Animation Events

Events are `AnimationEvent { f32 Time; std::string Name; }` stored on `AnimationClip::Events`.

During time advancement, `PreviousTime` is captured before advancing `CurrentTime`. After all evaluation jobs complete, a main-thread pass checks each event:

- **Normal forward** (`prev <= curr`): fire if `evtSec > prev && evtSec <= curr`
- **Loop wrap-around** (`prev > curr`): fire if `evtSec > prev || evtSec <= curr`

Callbacks are invoked via `Animation::OnAnimEvent` (`std::function<void(entt::entity, const std::string&)>`). Running on the main thread ensures safe ECS modification.

Events are currently not populated during import (Assimp doesn't expose event data natively). They will be authored in the editor (7E) or set at runtime.

### Bone Attachments

`BoneAttachment` component:

```
struct BoneAttachment {
    Entity TargetEntity;       // Entity with Animation component
    i32 BoneIndex = -1;        // Resolved at runtime
    std::string BoneName;      // Serialized; resolved via Skeleton::FindBone
    Vec3 LocalOffset;          // Translation offset in bone space
    Vec3 LocalRotation;        // Euler degrees offset in bone space
};
```

Post-evaluation pass (main thread, after events):

1. Lazy resolve `BoneName → BoneIndex` via `Skeleton::FindBone()` (cached after first resolution)
2. Compute `boneWorld = targetWorldTransform * globalBoneTransforms[boneIndex]`
3. Apply local offset: `finalMatrix = boneWorld * ComposeTransform(offset, rotation, 1)`
4. Decompose to `Transform` (Position, Rotation, Scale) + write directly to `WorldTransform.Matrix` to avoid one-frame lag (TransformSystem already ran this frame)

Serialization uses deferred UUID resolution (same pattern as parent hierarchy links in SceneSerializer).

### Memory Layout

Intermediate arrays per evaluation job: `localTransforms`, `globalTransforms`, `skinMatrices` — 3 × `vector<Mat4>` allocated on heap per entity. At MAX_BONES=256: 3 × 256 × 64 = 48KB per entity. Acceptable for small entity counts; future optimization could use LinearAllocator.

`GlobalBoneTransforms` persisted on Animation component: 256 × 64 = 16KB per animated entity (up to 2MB at 128 max entities).

## Editor Integration (Phase 7E)

### Animation Inspector (`editor/panels/InspectorPanel.cpp`)

Component-level UI in `DrawComponent<Animation>`:

- **Model auto-sync**: Copies `ModelUUID` from sibling MeshRenderer if unset
- **Clip selector**: Combo populated from `Model::GetAnimationClips()`
- **Transport**: Play/Pause/Stop buttons, Speed slider (0–5), Loop checkbox
- **Timeline scrubber**: SliderFloat 0→duration, sets `CurrentTime` for manual scrubbing when paused
- **Frame counter**: `time * ticksPerSecond` display
- **Show Bones**: Toggles `EditorSettings::showBoneDebug`

### Model Instantiation (`editor/panels/HierarchyPanel.cpp`)

`InstantiateModel()` creates the full entity hierarchy for skinned models:

```
Model (Animation comp)
├── Mesh1 (MeshRenderer comp)
├── Mesh2 (MeshRenderer comp)
└── RootBone
    ├── ChildBone1
    │   └── GrandchildBone
    └── ChildBone2
```

Animation component lives on the root entity only. MeshRenderer children reference the parent's bone block during rendering.

### Parent-Child Architecture

- **AnimationSystem** queries `<Animation, WorldTransform>` (no MeshRenderer required on root)
- **RenderingSystem** traverses parent entity for Animation component lookup — both shadow pass and geometry pass check `Parent` component to find `BoneBufferOffset`

### Bone Debug Overlay (`editor/panels/ScenePanel.cpp`)

`DrawBoneDebugOverlay()` renders skeleton wireframe:

1. Finds Animation on selected entity or its parent
2. Reads `GlobalBoneTransforms` from Animation component
3. Projects bone positions to 2D via View-Projection matrix
4. Draws parent-child lines with `ImDrawList`

### Assimp FBX Intermediate Nodes

`ModelImporter::ExtractSkeleton()` marks nodes targeted by animation channels as relevant, in addition to nodes referenced by bone inverse-bind-pose data. This handles Assimp's FBX decomposition nodes (`$AssimpFbx$_Translation`, `$AssimpFbx$_PreRotation`, etc.) which are targeted by animation channels but are not actual bone nodes.

## Blending & Root Motion (Phase 7D)

### AnimationController Component (`scene/AnimationController.h`)

Additive overlay on the `Animation` component. Entities with only `Animation` continue using the single-clip evaluation path from 7C unchanged.

```
AnimationController
├── Layers[]              — BlendLayer array, [0] = base layer
│   ├── ClipIndex         — Which animation clip to play
│   ├── CurrentTime       — Playback position (seconds)
│   ├── Speed, Loop       — Per-layer playback settings
│   ├── Weight            — Blend weight (0-1), base layer always 1.0
│   └── BoneMask[]        — Per-bone enable flags (empty = all bones)
├── ActiveTransition      — Optional crossfade state
│   ├── FromClip, ToClip  — Clips being blended
│   ├── Duration, Elapsed — Transition timing
│   └── FromTime/Speed/Loop — From-clip playback state
├── CurrentClipIndex      — Currently active clip
├── ApplyRootMotion       — Extract root bone delta and apply to Transform
├── DefaultTransitionDuration — Default crossfade length
└── RootMotionDelta       — Runtime: per-frame root motion (written by job, read by main thread)
```

### SQT Intermediate Format

Blending happens in Scale-Quaternion-Translation (SQT) space via `BonePose`:

```cpp
struct BonePose {
    Vec3 Position;
    Quat Rotation;
    Vec3 Scale;
};
```

`glm::mix` for position/scale, `glm::slerp` for rotation. Avoids matrix interpolation artifacts. Final Mat4 composed once after all blending via `ComposeTransform`.

### Blended Evaluation Flow

When `AnimationController` is present, `EvaluateBlended()` replaces the single-clip path:

1. **Crossfade** (if `ActiveTransition` active):
   - Sample "from" clip at `FromTime` → fromPoses
   - Sample "to" clip at base layer time → toPoses
   - `alpha = Elapsed / Duration`
   - `BlendPoses(fromPoses, toPoses, alpha)` → basePoses
   - Both clips continue advancing during crossfade

2. **No transition**: Sample base layer clip → basePoses

3. **Layered override** (layers 1+):
   - For each layer with weight > 0:
   - Sample layer clip → layerPoses
   - `BlendPoses(basePoses, layerPoses, layer.Weight, layer.BoneMask)` → basePoses
   - Masked-out bones are not affected

4. **Root motion extraction** (if `ApplyRootMotion && HasRootMotion`):
   - Compute root bone (index 0) position delta between previous and current time
   - Handle loop wrap boundary (two-segment delta)
   - Write XZ delta to `ctrl.RootMotionDelta` (Y stays on bone for vertical bob)
   - Zero root bone XZ translation to prevent double-movement

5. **Convert + propagate**: `PosesToLocalTransforms` → `PropagateAndUpload` (shared with single-clip path)

### Root Motion Application

Main thread, after all evaluation jobs complete, before bone attachments:

```
For each entity with AnimationController where ApplyRootMotion:
    worldDelta = entityRotation * RootMotionDelta
    transform.Position += worldDelta
```

Delta is computed in model space by the job, rotated by entity orientation on the main thread.

### Time Advancement

When `AnimationController` is present, the single-clip `Animation` time advancement is skipped. Instead:

- Each `BlendLayer.CurrentTime` advances independently (`dt * layer.Speed`, loop/clamp)
- `ActiveTransition.Elapsed` advances, `FromTime` advances with `FromSpeed`
- When `Elapsed >= Duration`, transition completes (reset to `nullopt`)
- `Animation.CurrentTime` and `AnimationIndex` are synced from base layer for event detection compatibility

### Serialization

Persisted fields: `CurrentClipIndex`, `ApplyRootMotion`, `DefaultTransitionDuration`, per-layer `ClipIndex/Speed/Weight/Loop/BoneMask`. Bone masks stored as sparse index arrays (only enabled bone indices). Runtime state (`ActiveTransition`, `RootMotionDelta`, layer `CurrentTime`) resets on load.

### Editor Integration

`DrawComponent<AnimationController>` in `InspectorPanel.cpp`:
- Current clip combo, root motion checkbox, transition duration slider
- Per-layer: clip combo, weight slider, speed slider, loop checkbox
- Override layers (1+): bone mask tree with per-bone checkboxes, All/None/Clear buttons
- Add/Remove layer controls
- "Animation Controller" in Add Component menu (only when `Animation` present)
