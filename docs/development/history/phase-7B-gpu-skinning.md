# Phase 7B — GPU Skinning Pipeline

**Date:** 2026-03-27

---

## Overview

Upload bone matrices via persistently-mapped SSBO (Set 4), create skinned shader variants for geometry and shadow passes, render skinned meshes in bind pose. Static meshes unaffected.

---

## BoneMatrixBuffer

**`renderer/BoneMatrixBuffer.h/.cpp`** — Static singleton following MaterialSystem pattern:

- **Layout**: 128 entity blocks x 256 bones x mat4 (64 bytes) = 2 MB
- **Descriptor**: Set 4, Binding 0, STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT
- **Memory**: VMA_MEMORY_USAGE_CPU_TO_GPU (host-visible, host-coherent), persistently mapped
- **API**: `AllocateBlock() -> u32 baseIndex`, `FreeBlock(u32)`, `UploadBones(u32 base, Mat4*, count)`
- Init fills entire buffer with identity matrices — skinned meshes render in bind pose without explicit uploads

---

## Descriptor Set Layout (5 sets)

```
Set 0: GlobalUniforms UBO + IBL samplers
Set 1: Bindless textures (16384)
Set 2: MaterialSystem SSBO
Set 3: Light UBO + shadow sampler
Set 4: BoneMatrixBuffer SSBO  <- NEW
```

All geometry/shadow/skybox pipelines use the 5-set layout. Skybox ignores Set 4.

---

## Push Constants

`ObjectPushConstants::_pad` repurposed as `boneOffset` (80 bytes total unchanged). Static meshes pass `boneOffset = 0` (ignored by non-skinned shader).

---

## Shader Variants

| Shader | Purpose | Inputs |
|--------|---------|--------|
| `pbr_skinned.vert` | Skinned geometry | locations 0-6 (84-byte stride), Set 4 SSBO |
| `shadowDepth_skinned.vert` | Skinned shadow | locations 0-6 (84-byte stride), Set 4 SSBO |

Both share fragment shaders with their static counterparts. Linear Blend Skinning (LBS): accumulate weighted bone matrices, apply before model transform, with identity fallback for unweighted vertices.

---

## Pipeline Selection

Two `PipelineManager` instances (static + skinned) with different vertex layouts captured in ConfigFactory closures. Per-mesh `MeshData::IsSkinned` flag determines which pipeline to bind. Shadow pass uses two separate `VKPipeline` objects.

---

## Files Modified

| File | Change |
|------|--------|
| `renderer/BoneMatrixBuffer.h` | **NEW** — SSBO allocation, descriptor, upload API |
| `renderer/BoneMatrixBuffer.cpp` | **NEW** — Vulkan buffer creation, slot management, identity init |
| `shaders/pbr_skinned.vert` | **NEW** — LBS skinning with SSBO bone lookup |
| `shaders/shadowDepth_skinned.vert` | **NEW** — Shadow pass skinning variant |
| `shaders/pbr.vert` | Added boneOffset to push constants (layout compat) |
| `shaders/pbr.frag` | Push constant layout update |
| `shaders/shadowDepth.vert` | Push constant layout update |
| `scene/systems/RenderingSystem.h` | DrawCommand isSkinned/boneOffset, SPIR-V vectors, Set 4 resources |
| `scene/systems/RenderingSystem.cpp` | 5-set pipeline layout, per-mesh pipeline selection, Set 4 binding |
