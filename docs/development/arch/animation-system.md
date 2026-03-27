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
  ├── ExtractSkeleton() — BFS from root, topological order
  ├── ProcessSkinnedMesh() — No transform baking, bone weights normalized
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
- **No transform baking for skinned meshes**: Vertex positions stay in mesh-local (bone) space. The skeleton hierarchy provides transforms at runtime.
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

During draw command collection, entities with skinned meshes get a bone block allocated via `BoneMatrixBuffer::AllocateBlock()`. Tracked in `m_BoneBlockMap` (ModelUUID → base index). Freed on RenderingSystem destruction.

## Runtime Evaluation (Phase 7C — planned)

AnimationSystem: keyframe sampling, hierarchy propagation, job-parallel per-entity evaluation.

## Blending (Phase 7D — planned)

AnimationController: crossfade, layered override with bone masks, root motion.
