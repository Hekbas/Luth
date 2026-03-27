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

## GPU Skinning (Phase 7B — planned)

BoneMatrixBuffer SSBO (Set 4), skinned pipeline variants, vertex shader LBS.

## Runtime Evaluation (Phase 7C — planned)

AnimationSystem: keyframe sampling, hierarchy propagation, job-parallel per-entity evaluation.

## Blending (Phase 7D — planned)

AnimationController: crossfade, layered override with bone masks, root motion.
