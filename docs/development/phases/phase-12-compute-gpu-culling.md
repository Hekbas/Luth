# Phase 12 — Compute Framework + GPU Frustum Culling

**Epic:** #55  |  **Version:** v1.2.0  |  **Est.:** Large (2-3 weeks)  |  **Deps:** —

---

## Goal

Extend the render graph with compute pass support. Implement GPU frustum culling via compute shader + indirect draw. Replace explicit per-object `vkCmdDrawIndexed` with `vkCmdDrawIndexedIndirect`. This is the keystone phase — every future GPU-driven feature (GTAO, Forward+, particles, HZB occlusion) depends on the compute infrastructure built here.

---

## Sub-Tasks and Commit Plan

### 12A: Render Graph Compute + Buffer Infrastructure

**Commit:** `feat(renderer): add compute pass and buffer resource support to render graph`
**Trailer:** `Part of #55`
**Issue items:**
- Add `ResourceState::ComputeRead`, `ComputeWrite`, `StorageBufferRead/Write`, `IndirectRead`
- Add `BufferDesc` struct + `BufferHandle` + `BufferBarrier` to `RenderGraphResources.h`
- Add `BufferNode` + `m_Buffers` vector + `AddComputePass<Data>()` template to `RenderGraph`
- Extend `GetStateInfo()` with compute stage/access mappings
- Buffer barrier emission (`VkBufferMemoryBarrier2`) in `SolveBarriers()` + `Execute()`
- Compute pass executes directly on primary cmd (no secondary cmd / BeginRendering)
- Storage buffer pooling in `RenderResourceCache`

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/renderer/rendergraph/RenderGraphResources.h` | EDIT | Add `BufferDesc`, `BufferHandle`, `BufferBarrier`. Add 5 new `ResourceState` values. |
| `luth/source/luth/renderer/rendergraph/RenderGraph.h` | EDIT | Add `BufferNode`, `PassNode::isCompute`, buffer read/write vectors, `bufferPreBarriers`. `AddComputePass<Data>()` template. Builder: `ReadBuffer()`, `WriteBuffer()`, `ReadIndirectBuffer()`, `ReadStorageImage()`, `WriteStorageImage()`. Context: `GetBuffer`. Internal API: `RegisterBuffer()`, `ImportBuffer()`, `RegisterBufferRead()`, `RegisterBufferWrite()`. |
| `luth/source/luth/renderer/rendergraph/RenderGraph.cpp` | EDIT | Extend `GetStateInfo()`/`GetLayout()` for compute states. Extend `SolveBarriers()` for buffer resources. Extend `Execute()`: combined image+buffer barrier emission; compute pass direct-on-primary path. Extend `CullDeadPasses()`, `ComputeLifetimes()`, `AllocatePhysicalResources()`, `CleanupPhysicalResources()` for buffers. |
| `luth/source/luth/renderer/rendergraph/RenderResourceCache.h` | EDIT | Add `PooledBuffer` struct. Add `GetBuffer()`, `ReturnBuffer()`. |
| `luth/source/luth/renderer/rendergraph/RenderResourceCache.cpp` | EDIT | Implement buffer pooling (VulkanAllocator). Extend Shutdown/GC. |

**Verify:**
- [ ] Build succeeds (no errors, no new warnings)
- [ ] Existing render passes still work unchanged (no visual regression)
- [ ] No Vulkan validation errors

---

### 12B: VulkanComputePipeline Wrapper

**Commit:** `feat(renderer): add VulkanComputePipeline wrapper`
**Trailer:** `Part of #55`
**Issue items:**
- Create `VulkanComputePipeline` wrapper (`backend/vulkan/VulkanComputePipeline.h/cpp`)

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/renderer/backend/vulkan/VulkanComputePipeline.h` | NEW | Class: `VKComputePipeline`. Constructor takes SPIR-V, descriptor set layouts, push constant ranges. `Bind(VkCommandBuffer)`. `GetLayout()`. |
| `luth/source/luth/renderer/backend/vulkan/VulkanComputePipeline.cpp` | NEW | `vkCreateComputePipelines` + pipeline layout. Destructor cleans up. |

**Verify:**
- [ ] Build succeeds
- [ ] No Vulkan validation errors

---

### 12C: GPU Object Buffer + Cull Compute Pass

**Commit:** `feat(renderer): add GPU object buffer and frustum cull compute pass`
**Trailer:** `Part of #55`
**Issue items:**
- Define `GPUObjectData` struct
- Per-frame upload of all renderable objects to GPU SSBO
- Write `gpu_cull.comp` compute shader
- Add cull compute pass to render graph (before GeometryPass)

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/renderer/DrawCommand.h` | EDIT | Add `GPUObjectData` struct (model, boundingSphere, materialIndex, shadeMode, entityID, boneOffset, indexCount, firstIndex, vertexOffset, pad). |
| `luth/assets/shaders/gpu_cull.comp` | NEW | 256 invocations/group. Input: GPUObjectData SSBO. Output: sets `instanceCount` in pre-built `VkDrawIndexedIndirectCommand[]`. Push constants: 6 frustum planes + object count. |
| `luth/source/luth/renderer/passes/CullPass.h` | NEW | `AddCullComputePass()` function declaration. |
| `luth/source/luth/renderer/passes/CullPass.cpp` | NEW | Compute pass using `AddComputePass`. Binds object SSBO + indirect buffer. Pushes frustum planes. Dispatches. |
| `luth/source/luth/scene/systems/RenderingSystem.h` | EDIT | Add members: `m_ObjectSSBO`, `m_IndirectBuffer`, cull pipeline, descriptor set/layout. Add `BuildGPUObjectBuffer()`. |
| `luth/source/luth/scene/systems/RenderingSystem.cpp` | EDIT | Before shadow pass: collect renderables into `GPUObjectData[]`, upload to SSBO, build initial indirect commands (instanceCount=1 per object), call `AddCullComputePass()`. |

**Design:** Each indirect command's `firstInstance` = object index into SSBO. GPU cull sets `instanceCount=0` for culled objects (no compaction needed).

**Verify:**
- [ ] Build succeeds
- [ ] Cull pass visible in frame debugger pass list
- [ ] No Vulkan validation errors
- [ ] GPU object buffer uploads correctly

---

### 12D: Indirect Draw Conversion (GeometryPass + Shaders)

**Commit:** `feat(renderer): convert GeometryPass to indirect draw with per-object SSBO`
**Trailer:** `Part of #55`
**Issue items:**
- Convert GeometryPass from `vkCmdDrawIndexed` to `vkCmdDrawIndexedIndirect`
- Modify PBR vertex shader to read per-object data from SSBO via `gl_BaseInstance`

| File | Change | Notes |
|------|--------|-------|
| `luth/assets/shaders/pbr.vert` | EDIT | Remove model/mat/shadeMode/entityID/boneOffset from push constants. Add `layout(std430, set=5, binding=0) readonly buffer ObjectBuffer`. Read via `objects[gl_BaseInstance]`. |
| `luth/assets/shaders/pbr_skinned.vert` | EDIT | Same changes. `boneOffset` from SSBO. |
| `luth/assets/shaders/pbr.frag` | EDIT | `materialIndex` and `entityID` received as varyings from vert (not push constants). |
| `luth/source/luth/renderer/passes/GeometryPass.cpp` | EDIT | Replace per-object draw loop. Group by (VB, IB, pipeline). Per group: bind VB/IB, call `vkCmdDrawIndexedIndirect`. Remove per-draw push constants. |
| `luth/source/luth/renderer/passes/GeometryPass.h` | EDIT | Add Set 5 descriptor set for object SSBO. |
| `luth/source/luth/renderer/PipelineManager.h` | EDIT | Update push constant layout. Add Set 5 to descriptor set layouts. |

**Verify:**
- [ ] Scene renders identically to pre-indirect baseline
- [ ] Mouse picking (entityID) still works
- [ ] Skinned meshes animate correctly
- [ ] No Vulkan validation errors

---

### 12E: IBLPrecompute Refactor

**Commit:** `refactor(renderer): migrate IBLPrecompute to VulkanComputePipeline`
**Trailer:** `Part of #55`
**Issue items:**
- Refactor `IBLPrecompute.cpp` to use new `VulkanComputePipeline` class

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/renderer/IBLPrecompute.cpp` | EDIT | Replace `RunComputeDispatch()` with persistent `VKComputePipeline` instances per operation. Remove ad-hoc per-call `vkCreateShaderModule`/`vkCreateComputePipelines`. |

**Verify:**
- [ ] IBL still works (skybox + PBR ambient correct)
- [ ] No Vulkan validation errors

---

### 12F: ShadowPass Indirect + Frame Debugger + v1.2.0

**Commit:** `feat(renderer): convert ShadowPass to indirect draw, update frame debugger`
**Trailer:** `Closes #55`
**Issue items:**
- Convert ShadowPass to indirect draw (shared cull results)
- Frame debugger: capture indirect draw calls
- Verify: no validation errors, correct output matches baseline
- Bump to v1.2.0

| File | Change | Notes |
|------|--------|-------|
| `luth/source/luth/renderer/passes/ShadowPass.cpp` | EDIT | Indirect draw using shared main-camera cull results. |
| `luth/assets/shaders/shadowDepth.vert` | EDIT | Read model matrix from object SSBO instead of push constants. |
| `luth/assets/shaders/shadowDepth_skinned.vert` | EDIT | Same. |
| `luth/source/luth/renderer/FrameDebugger.h` / `.cpp` | EDIT | Update `CapturedDrawCall` to record indirect draw metadata. |
| `luth/source/luth/editor/panels/FrameDebuggerPanel.cpp` | EDIT | Display indirect draw info (draw count, culled objects). |
| `luth/source/luth/core/Version.h` | EDIT | Bump to `v1.2.0`. |

**Verify:**
- [ ] Shadows render correctly
- [ ] Frame debugger captures and displays compute + indirect passes
- [ ] Full scene renders identically to pre-Phase-12 baseline
- [ ] No Vulkan validation errors
- [ ] Performance: fewer draw calls in profiler

---

## Architecture Notes

### New ResourceState Values
```cpp
ComputeRead,        // COMPUTE_SHADER | SHADER_READ   (storage image read)
ComputeWrite,       // COMPUTE_SHADER | SHADER_WRITE  (storage image write)
StorageBufferRead,  // COMPUTE_SHADER | SHADER_READ   (SSBO read)
StorageBufferWrite, // COMPUTE_SHADER | SHADER_WRITE  (SSBO write)
IndirectRead,       // DRAW_INDIRECT  | INDIRECT_COMMAND_READ (indirect buffer)
```

### GPUObjectData (std430, 112 bytes)
```cpp
struct GPUObjectData {
    glm::mat4 model;          // 64B
    glm::vec4 boundingSphere; // 16B — xyz=center, w=radius (local space)
    u32 materialIndex;        // 4B
    u32 shadeMode;            // 4B
    u32 entityID;             // 4B
    u32 boneOffset;           // 4B
    u32 indexCount;           // 4B
    u32 firstIndex;           // 4B
    i32 vertexOffset;         // 4B
    u32 _pad;                 // 4B
};
```

### Descriptor Sets (after Phase 12)
| Set | Content |
|-----|---------|
| 0   | GlobalUniforms UBO + shadowMap + IBL |
| 1   | Bindless textures |
| 2   | Material SSBO |
| 3   | Light UBO |
| 4   | Bone matrices SSBO |
| 5   | **GPUObjectData SSBO (NEW)** |

### Indirect Draw Flow
```
CPU: GPUObjectData[] sorted by (VB, IB, pipeline) → upload to SSBO
CPU: VkDrawIndexedIndirectCommand[] with instanceCount=1 → upload
GPU cull: test bounding sphere vs 6 frustum planes → set instanceCount=0 if culled
GPU draw: per (VB, IB, pipeline) group → vkCmdDrawIndexedIndirect
           instanceCount=0 commands are skipped by hardware
```

### Existing Utilities Reused
- `Math.h:129` — `CreateFrustumFromCamera()` (Gribb-Hartmann plane extraction)
- `MeshData::BindPoseAABB` (`Model.h:42`) — pre-computed per-mesh bounds
- `Animation::AnimatedAABB` (`Components.h:115`) — per-frame world-space bounds
- `IBLPrecompute.cpp` — dispatch pattern reference (prior art, replaced in 12E)

---

## References

- `docs/development/ROADMAP_TODO.md` — Phase 12 section
- `docs/development/TECHNICAL_DEEPDIVE.md` — Section 1: Frustum Culling
- `docs/development/arch/rendering-pipeline.md` — Descriptor sets, pass order
- `scripts/issues/phase-12.md` — GitHub issue checklist
- Prior art: `luth/source/luth/renderer/IBLPrecompute.cpp` — `RunComputeDispatch()`

---

## Progress Tracker

| Sub-Task | Status | Commit | Date |
|----------|--------|--------|------|
| 12A: Render graph compute + buffer infrastructure | in-progress | — | — |
| 12B: VulkanComputePipeline wrapper | pending | — | — |
| 12C: GPU object buffer + cull compute pass | pending | — | — |
| 12D: Indirect draw conversion (GeometryPass) | pending | — | — |
| 12E: IBLPrecompute refactor | pending | — | — |
| 12F: ShadowPass + frame debugger + v1.2.0 | pending | — | — |
