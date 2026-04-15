# Phase 12 — Compute Framework + GPU Frustum Culling

**Date:** 2026-04-13 – 2026-04-15
**Version:** v1.2.0
**Epic:** #55
**Commits:** 6 (12A–12F)

---

## Overview

GPU-driven rendering infrastructure: compute pass support in the render graph, GPU frustum culling via a compute shader + indirect draw, and migration of every graphics pass off `vkCmdDrawIndexed` to `vkCmdDrawIndexedIndirect`. This is the keystone phase that unlocks GTAO, Forward+, HZB occlusion, and GPU particles in later phases.

---

## Commit Breakdown

### 12A — Render Graph Compute + Buffer Infrastructure

Extended the render-graph DAG with compute pass support and typed buffer resources.

| File | Action |
|------|--------|
| `renderer/rendergraph/RenderGraphResources.h` | EDIT — `BufferDesc`, `BufferHandle`, `BufferBarrier`; 5 new `ResourceState` values (`ComputeRead/Write`, `StorageBufferRead/Write`, `IndirectRead`) |
| `renderer/rendergraph/RenderGraph.h` | EDIT — `BufferNode`, `PassNode::isCompute`, `AddComputePass<Data>()`, builder: `ReadBuffer/WriteBuffer/ReadIndirectBuffer`, context: `GetBuffer` |
| `renderer/rendergraph/RenderGraph.cpp` | EDIT — compute state mappings in `GetStateInfo()`, `VkBufferMemoryBarrier2` emission in `SolveBarriers()`, compute-direct-on-primary path in `Execute()` |
| `renderer/rendergraph/RenderResourceCache.h/.cpp` | EDIT — `PooledBuffer` + `GetBuffer/ReturnBuffer` (VMA-backed storage buffer pooling) |

### 12B — VulkanComputePipeline Wrapper

| File | Action |
|------|--------|
| `renderer/backend/vulkan/VulkanComputePipeline.h` | NEW — `VKComputePipeline`: constructor takes SPIR-V + layouts + push ranges; `Bind()`, `GetLayout()` |
| `renderer/backend/vulkan/VulkanComputePipeline.cpp` | NEW — `vkCreateComputePipelines` + pipeline layout; destructor cleans up |

### 12C — GPU Object Buffer + Cull Compute Pass

| File | Action |
|------|--------|
| `renderer/DrawCommand.h` | EDIT — `GPUObjectData` struct (112 bytes, std430) |
| `assets/shaders/gpu_cull.comp` | NEW — 256 invocations/group; input: GPUObjectData SSBO; output: sets `instanceCount=0` in indirect command array; push constants: 6 frustum planes + object count |
| `renderer/passes/CullPass.h/.cpp` | NEW — `AddCullComputePass()` free function |
| `scene/systems/RenderingSystem.h/.cpp` | EDIT — `m_ObjectSSBO`, `m_IndirectBuffer`, cull pipeline + descriptor set, `BuildGPUObjectBuffer()`, `m_EntityToSSBOIndex` map |

### 12D — Indirect Draw Conversion (GeometryPass + Shaders)

| File | Action |
|------|--------|
| `assets/shaders/pbr.vert` | EDIT — removed push constants; reads `objects[gl_BaseInstance]` from Set 5 SSBO |
| `assets/shaders/pbr_skinned.vert` | EDIT — same; `boneOffset` from SSBO |
| `assets/shaders/pbr.frag` | EDIT — `materialIndex` and `entityID` from varyings |
| `renderer/passes/GeometryPass.cpp` | EDIT — per-object draw loop → `vkCmdDrawIndexedIndirect` grouped by (VB, IB, pipeline); Set 5 bound |
| `scene/systems/RenderingSystem.cpp` | EDIT — geometry pipeline layouts updated to 6 sets (geoLayouts), push constants removed from geo pipelines |

### 12E — IBLPrecompute Refactor

| File | Action |
|------|--------|
| `renderer/IBLPrecompute.cpp` | EDIT — replaced ad-hoc per-call `vkCreateComputePipelines` with persistent `VKComputePipeline` instances per IBL operation |

### 12F — ShadowPass Indirect + Frame Debugger + v1.2.0

| File | Action |
|------|--------|
| `assets/shaders/shadowDepth.vert` | EDIT — removed push constants; reads `objects[gl_BaseInstance].model` from Set 5 SSBO |
| `assets/shaders/shadowDepth_skinned.vert` | EDIT — same; `boneOffset` from SSBO; LBS uses `obj.boneOffset` |
| `scene/systems/RenderingSystem.cpp` | EDIT — shadow pipelines use `geoLayouts` (6 sets), `pushConstantRanges = {}`; `hIndirectBuf` threaded into `AddShadowPass` |
| `renderer/passes/ShadowPass.cpp` | EDIT — replaced per-object direct draw loop with `vkCmdDrawIndexedIndirect` using shared cull results; binds Set 5 |
| `renderer/rendergraph/FrameCapture.h` | EDIT — `DispatchKind` enum; `CapturedDrawCall` extended with `kind`, indirect offset/count/stride, compute group counts |
| `renderer/FrameDebugger.h/.cpp` | EDIT — `CaptureIndirectDraw()` and `CaptureComputeDispatch()` |
| `renderer/passes/CullPass.h/.cpp` | EDIT — optional `FrameDebugger*` param; FrustumCull appears in frame capture |
| `renderer/passes/GeometryPass.cpp` | EDIT — switched from `CaptureDrawCall` to `CaptureIndirectDraw` |
| `editor/panels/FrameDebuggerPanel.cpp` | EDIT — `[C]`/`[I]` prefixes in tree; Kind row + dispatch/indirect metadata in detail view; Transform/PushConstants hidden for compute entries |
| `core/Version.h` | EDIT — v1.1.1 → v1.2.0 |

---

## Architecture Summary

### Descriptor Sets (final after Phase 12)
| Set | Content |
|-----|---------|
| 0 | GlobalUniforms UBO + shadowMap + IBL |
| 1 | Bindless textures |
| 2 | Material SSBO |
| 3 | Light UBO |
| 4 | Bone matrices SSBO |
| 5 | GPUObjectData SSBO (new) |

### Indirect Draw Flow
```
CPU: GPUObjectData[] sorted by (VB, IB, pipeline) → upload to SSBO
CPU: VkDrawIndexedIndirectCommand[] with instanceCount=1 → upload
GPU cull: test bounding sphere vs 6 frustum planes → set instanceCount=0 if culled
GPU draw: per (VB, IB, pipeline) group → vkCmdDrawIndexedIndirect
           instanceCount=0 commands are skipped by hardware
```

---

## Key Decisions

1. **ShadowPass shares main-camera cull results** — ShadowPass reuses the same `m_IndirectBuffer` that the GPU cull compute pass writes for the main camera frustum. Objects outside the main frustum that cast visible shadows are incorrectly culled. Accepted tradeoff for v1.2.0; per-cascade shadow culling deferred to Phase 13 (Cascaded Shadow Maps).

2. **No draw command compaction** — Culled objects have `instanceCount=0`; hardware skips them with no GPU-side compaction step. This avoids an `atomicAdd`-based compaction pass and is sufficient until object counts exceed ~10k.

3. **`gl_BaseInstance` as SSBO index** — `firstInstance` in each `VkDrawIndexedIndirectCommand` is set to the object's SSBO index at CPU upload time. The vertex shader reads `objects[gl_BaseInstance]` to get per-object data. No `gl_InstanceIndex` remapping needed.

4. **Shadow pipelines reuse `geoLayouts`** — Rather than creating a separate 6-set layout for shadow, the existing `geoLayouts` vector (built for geometry pipelines) is shared. Clean and zero duplication.
