# Phase 7A — Data Extraction & Skinned Vertex Format

**Date:** 2026-03-27

---

## Overview

Extract skeleton hierarchy, bone weights, and animation clips from Assimp. Introduce `SkinnedVertex` (84 bytes) alongside `Vertex` (52 bytes). V2 binary serialization format with backward-compatible V1 deserialization. No runtime animation or GPU skinning yet — data structures and import pipeline only.

---

## Data Structures

**`renderer/Skeleton.h`** — Bone hierarchy in topological order (BFS from root):

- `BoneInfo`: Name, ParentIndex, InverseBindPose (from `aiBone::mOffsetMatrix`), LocalBindPose (from `aiNode::mTransformation`)
- `Skeleton`: Bones vector + BoneNameToIndex map + FindBone lookup
- Constants: `MAX_BONES = 256`, `MAX_BONES_PER_VERTEX = 4`

**`renderer/AnimationClip.h`** — Per-channel keyframe storage:

- `BoneTrack`: BoneIndex + separate Position/Rotation/Scale keyframe vectors (independent counts, binary search friendly)
- `AnimationClip`: Name, Duration (ticks), TicksPerSecond, Tracks, Events, HasRootMotion flag
- `AnimationEvent`: Time + Name (authored in editor or set at runtime)

**`renderer/Model.h`** — Dual vertex format:

- `SkinnedVertex` (84 bytes): extends `Vertex` (52 bytes) with `ivec4 BoneIDs` + `vec4 BoneWeights`
- `MeshData::IsSkinned` flag selects which vertex vector is populated and which GPU layout is used

---

## Assimp Extraction Pipeline

**`resources/importers/ModelImporter.cpp`** — Three new extraction functions:

1. **`ExtractSkeleton()`**: Collect bone names from `aiMesh::mBones`, store `mOffsetMatrix` as InverseBindPose, BFS walk of `aiScene->mRootNode` for topological order. Non-bone ancestor nodes included as structural entries. Axis correction applied to skeleton root transform (NOT per-vertex) to prevent double-application during hierarchy propagation.

2. **`ProcessSkinnedMesh()`** (was `ExtractBoneWeights`): Per-vertex bone weight accumulation (max 4), normalization to sum 1.0. Unweighted vertices bind to root bone.

3. **`ExtractAnimationClips()`**: Per-animation channel iteration, BoneNameToIndex lookup, keyframe copy per channel. Default ticks/sec = 25 if source reports 0.

Added `aiProcess_LimitBoneWeights` to Assimp import flags.

---

## Binary Serialization

**`resources/AssetSerializer.cpp`** — V2 format:

- `AssetHeader::Version >= 2` indicates skeleton data present
- Serializes: skeleton bones (name, parent index, inverse bind pose, local bind pose) + animation clips (tracks with per-channel keyframes)
- V1 files still load as static meshes (backward compatible)
- `Model::ProcessMeshData()` selects vertex layout (Float3+Float3+Float2+Float2+Float3 vs same + Int4+Float4) based on `IsSkinned`

---

## Key Design Decisions

- **Separate `SkinnedVertex` vs extending `Vertex`**: Two structs. Static meshes (majority of geometry) stay at 52 bytes. Skinned meshes pay 84 bytes. Avoids wasting 32 bytes per vertex on environments/props.
- **Axis correction on skeleton root**: Applied to root bone's LocalBindPose, not per-vertex. Prevents double-application when bone transforms propagate through hierarchy.
- **Topological bone ordering**: BFS from root guarantees parent before child. Enables single forward pass during hierarchy propagation (7C).

---

## Files Modified

| File | Change |
|------|--------|
| `renderer/Skeleton.h` | **NEW** — BoneInfo, Skeleton data structures |
| `renderer/AnimationClip.h` | **NEW** — VectorKey, QuatKey, BoneTrack, AnimationClip, AnimationEvent |
| `renderer/Model.h` | SkinnedVertex struct, Skeleton/AnimationClip storage, accessors |
| `renderer/Model.cpp` | ProcessMeshData skinned vertex layout path |
| `resources/importers/ModelImporter.h` | ModelAssetData: Skeleton, AnimationClips, IsSkinned fields |
| `resources/importers/ModelImporter.cpp` | ExtractSkeleton, ProcessSkinnedMesh, ExtractAnimationClips |
| `resources/AssetSerializer.h` | V2 format declarations |
| `resources/AssetSerializer.cpp` | V2 binary serialize/deserialize with skeleton + clips |
| `resources/AssetManager.cpp` | Updated model load path |
