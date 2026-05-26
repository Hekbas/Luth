# rt-renderer.B.2 — blas-tlas

**Date:** 2026-05-26
**Commits:** 6 (on `feat/blas-tlas`)
**Issue:** [#138](https://github.com/Hekbas/Luth/issues/138)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127)
**Series:** `rt-renderer`, Phase B.2. Mode A series-coalesced — `Version.h` PATCH bumps `v3.0.8` → `v3.0.9`, tag-only, no Release.

---

## Overview

First effort in the arc that **builds and consumes acceleration structures**. Per-mesh BLAS built at `Model::ProcessMeshData` for both static and skinned meshes; skinned meshes additionally allocate a per-mesh persistent "deformed positions" buffer plus a tight-packed "skin input" buffer (position + bone IDs + weights). A new compute shader (`skinning.comp`) reads the bone matrices from the existing `BoneMatrixBuffer` SSBO and writes deformed positions per frame, feeding a `MODE_UPDATE_KHR` AS refit. Per-frame TLAS rebuild from `RenderSnapshot::meshes` with hash-based dirty short-circuit (skips the rebuild call when entity set + translation hash matches the previous frame; the prior TLAS handle + storage stay alive across frames in that case). Set 0 grows from 6 → 7 bindings — binding 6 is the per-frame TLAS handle, written via `VkWriteDescriptorSetAccelerationStructureKHR` pNext-chained into the existing `GlobalSubsystem::UpdateUBO` batched write. No RT shader consumes the TLAS yet — the smoke check is RenderDoc capture showing `vkCmdBuildAccelerationStructuresKHR` running on the AsyncCompute primary plus the descriptor populated with the right device address; B.3 brings the first reader.

Plan-mode review found and corrected three significant claims that would have shipped incorrectly:

- **AS-build queue requirement** — the umbrella spec said "BLAS/TLAS builds require graphics queue family in current Vulkan." The Vulkan 1.3 spec actually requires only `VK_QUEUE_COMPUTE_BIT` on the command pool. Graphics families always advertise compute (so import-time `ImmediateSubmit` on the graphics queue works), but compute-only family is also legal. NVIDIA's RTX best-practices explicitly recommends async-compute for AS building. B.2 routes `TlasBuildPass` to `QueueFamily::AsyncCompute` to overlap with the rest of the graphics frame. The wrong claim is corrected in [`epics/rt-renderer.md`](../../epics/rt-renderer.md) + a new paragraph in [`arch/multi-queue.md`](../../arch/multi-queue.md).
- **BLAS-build flags** — added `VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR` (static + skinned) per NVIDIA — BLAS quality outweighs build cost since builds are amortized over thousands of trace rays.
- **Unique scratch per batched BLAS build** — NVIDIA's rule "all BLAS build calls need unique scratch memory" means batched skinned-BLAS refits cannot share scratch. `TlasBuilder::RefitSkinnedBLASes` allocates one tagged-heap scratch sized to `Σ updateScratchSize` and slices into per-mesh sub-regions.

The full plan-mode review (and the three open-decision pivots resolved with the user — fold per-frame skinned BLAS refit in v3.0.9 rather than defer, fold hash-based TLAS dirty skip in v3.0.9, route everything to AsyncCompute inside the RG) is in the (untracked) plan file under `~/.claude/plans/`. The folding-in decisions grew B.2 from the originally-scoped 6 sub-tasks (static BLAS only + always-rebuild TLAS + outside-RG ImmediateSubmit) to the 6 shipped here, with the skinned + dirty + RG-routing work absorbed without growing the sub-task count.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| A | **VB/IB AS build-input flag + upload-fence accessor.** `VKVertexBuffer`/`VKIndexBuffer` ctors add `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR` (required by `vkCmdBuildAccelerationStructuresKHR` per VUID-pInfos-03671/03672). Both stash the `UploadContext` fence value at construction + expose `GetUploadFence()` so the BLAS factory can `WaitForUpload(maxFence)` before recording the build — VB/IB upload runs on the transfer-queue submission chain, and the graphics-queue draws' implicit serialize does not cover the AS-build cmd buffer. | `86704ad` |
| B | **`VKAccelerationStructure` RAII + static BLAS factory + per-mesh ownership.** New `luth/source/luth/renderer/backend/vulkan/VulkanAccelerationStructure.{h,cpp}`: handle + persistent storage buffer + cached device address; dtor pushes both to `VulkanContext::PushDeletion` (drains N+2). `CreateStaticBLAS(const Mesh&)` factory gates on VB/IB upload fence, queries build sizes, allocates AS storage + scratch (one-shot VMA + `PushDeletion`), runs `ImmediateSubmit` with `PREFER_FAST_TRACE` flag. `Mesh` gained `shared_ptr<VKAccelerationStructure> m_Blas` + `GetBlas/SetBlas` + `m_VertexCount` + `m_IsSkinned` members; `Mesh::Create` no longer builds the BLAS itself — `Model::ProcessMeshData` becomes the single call site so static + skinned paths stay uniform. | `652d9fc` |
| C | **Skinned BLAS path: compute-skin + deformed-VB + ALLOW_UPDATE + refit.** `VKAccelerationStructure` extended with skinned-only members (`m_SkinInputBuffer` + `m_DeformedBuffer` + their VMA allocs + cached BDAs + cached `m_UpdateScratchSize`), `CreateSkinnedBLAS` factory, and `Refit` member method. `SkinComputeInput` POD (Vec4 position + IVec4 boneIDs + Vec4 weights = 48 B) locked to GLSL std430 via `static_assert`. New `luth/assets/shaders/skinning.comp` reads input/output via `GL_EXT_buffer_reference` + push-constant BDAs; bone matrices via set 0 binding 0 (BoneMatrixBuffer SSBO — its descriptor binding stageFlags gain `COMPUTE_BIT`). New `SkinningSubsystem` owns the compute pipeline + hot-reload hook. Registered as the 10th subsystem on `RenderPipeline`. | `ad28d24` |
| D | **`TlasBuildPass` with hash-skip + AS-state RG enums.** New `luth/source/luth/renderer/backend/vulkan/TlasBuilder.{h,cpp}` — `BuildTlas` (FNV-1a hash short-circuit, instance-buffer pack with row-major `VkTransformMatrixKHR` transpose, per-frame VMA storage + tagged scratch, build cmd record) + `RefitSkinnedBLASes` (one batched `vkCmdBuildAccelerationStructuresKHR` call with per-mesh scratch sub-regions sliced from a single tagged allocation per NVIDIA's "unique scratch" rule). `RtSubsystem::AddTlasBuildPass` on `QueueFamily::AsyncCompute` orchestrates: `SkinningSubsystem::DispatchAllSkinned` → compute-write → AS-build-read memory barrier → batched refit → AS-build-write → AS-build-read memory barrier → TLAS build with hash-skip. Multi-view guard (`m_LastBuildFrame == frameAbs`) short-circuits the second view since TLAS is scene-global. `RG::ResourceState::AccelerationStructure{Build,Read}` enum entries + `GetStateInfo` switch arms added — no B.2 consumer declares the Read yet; lands now so B.3's RT shadow pass can use the RG barrier solver natively. | `d295bd0` |
| E | **Set 0 binding 6 TLAS write + Execute hook.** `GlobalSubsystem` Set 0 layout 6 → 7 bindings; binding 6 = `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` with `UPDATE_AFTER_BIND_BIT | PARTIALLY_BOUND_BIT` (pre-scene null-handle safety). `UpdateUBO` batched write extends from `writes[2]` to `writes[3]` — TLAS handle pulled from `RtSubsystem::GetTlas()`, pNext-chained via `VkWriteDescriptorSetAccelerationStructureKHR`. `ViewResources::AllocateViewResources` pool gains an `ACCELERATION_STRUCTURE_KHR` slot (`k_ViewPoolAccelStructCount = 8`) to satisfy the Set 0 allocation. `RenderPipeline::Execute` slots `m_Rt.AddTlasBuildPass(rg)` between `AddResolvePass` (volumetric) and `AddPrefilterPass` (GTAO). | `9932353` |
| F | **Wrap-up + Version.h bump.** This history file. `arch/rendering-pipeline.md` Set 0 row bumped 6 → 7; B.1 deferral footnote removed + B.2 description added. `arch/multi-queue.md` gains a paragraph documenting AS-build queue semantics. `epics/rt-renderer.md` Progress Tracker B.2 row → done; B.2 entry text corrected from "graphics queue (BLAS/TLAS builds require graphics queue family in current Vulkan)" → "AsyncCompute queue". **`Version.h` bumped 3.0.8 → 3.0.9.** `--no-ff` merge into `main` + `v3.0.9` tag, no Release (Mode A intermediate). | this commit |

---

## Architectural decisions

### AsyncCompute routing for AS builds

`vkCmdBuildAccelerationStructuresKHR` requires `VK_QUEUE_COMPUTE_BIT` on the command pool per the Vulkan 1.3 spec — not `VK_QUEUE_GRAPHICS_BIT`. Graphics families always advertise compute, so import-time `ImmediateSubmit` on the graphics queue (`VulkanContext.cpp:600` — uses `m_GraphicsQueue`) works for the synchronous BLAS-at-import path. Per-frame TLAS build + skinned-BLAS refit go through the RenderGraph on `QueueFamily::AsyncCompute` to overlap with the graphics frame. The cross-queue handoff to a future B.3 RT consumer is covered by the per-submit timeline semaphore wait at the next graphics submit plus RG's `TOP_OF_PIPE` substitution for cross-queue barriers (already in place per `arch/multi-queue.md`).

### Static + skinned BLAS distinction lives on `VKAccelerationStructure`

Considered three layering shapes:

1. Separate `MeshSkinningResources` class in `luth/renderer/backend/vulkan/` referenced by `shared_ptr` from `Mesh`. Cleaner separation but spreads RT-per-mesh state across two classes.
2. Members on `Mesh` directly. Couples `Mesh.h` (in `resources/`) to backend Vulkan headers — layering violation.
3. **Skinned-only members on `VKAccelerationStructure` plus an `IsSkinned()` predicate.** All per-mesh RT GPU state in one RAII container; `Mesh.h` only forward-declares the class. Picked this for B.2 — keeps the Mesh class minimal (3-line additions) and the AS class' dtor PushDeletion handles all 3 owned buffers (storage + skin-input + deformed) in one site.

### Initial skinned BLAS over zero-init deformed positions

The first per-frame skinning compute fills the deformed-VB; the initial AS build at `CreateSkinnedBLAS` time sees all-zero positions, producing a degenerate-bounds BLAS. Acceptable because no B.2 shader consumes the TLAS — by the time B.3's first reader runs, at least one frame's compute-skin + AS refit has filled real positions. An alternative initial path (skin once at import with identity bone matrices) would have required wiring the skinning compute through `ImmediateSubmit` at import time, with bone matrices not yet allocated. The zero-init path keeps the import path linear: allocate buffers, run AS build, done.

### Hash-based TLAS dirty short-circuit (translation-only)

`HashInstances` is FNV-1a over `entity` + the 4th column translation bytes + `meshIndex`. Rotation-only changes on a non-translating mesh produce false-positive matches (skipped rebuild). Considered full 64-byte matrix hash — chose translation-only for cheaper hot path (≤10 µs for 100 instances vs ~30 µs for full matrix). If RT shadows on a rotating-only mesh look stale, bump to full-matrix; cost is still negligible at the scene sizes the engine targets. Documented in the function's prologue.

### Single global memory barrier between skinning + refit, refit + TLAS build

NVIDIA's guidance: "use a single global UAV barrier before TLAS build" rather than per-resource barriers. Two `VkMemoryBarrier2`s in `RtSubsystem::AddTlasBuildPass` execute body:

1. `ComputeShader` write → `AccelerationStructureBuild` read — covers every skinned mesh's deformed-VB at once.
2. `AccelerationStructureBuild` write → `AccelerationStructureBuild` read — covers all refitted BLAS handles before the TLAS reads them through the instance buffer.

The RG handles cross-pass barriers for resources declared via `Read`/`Write`, but the AS-build flow lives entirely inside the pass body (no RG-tracked resources for B.2 — instance buffer + scratch + AS storage all live outside the RG, retired via `PushDeletion` / tagged-heap `FreeTag(N-2)`). One pass + inline barriers is the simpler shape; when B.3 adds an RT consumer that reads the TLAS as an RG resource, the `AccelerationStructureRead` enum entry (added in B.2.D) lets the RG handle that handoff natively.

### Per-frame VMA for TLAS storage (NOT tagged-heap backing extension)

`GPUTaggedPageAllocator` backings have a fixed universal usage flag set (`STORAGE | INDIRECT | UNIFORM | VERTEX | SHADER_DEVICE_ADDRESS`). Adding `ACCELERATION_STRUCTURE_STORAGE_BIT_KHR` would propagate the bit to every Light/Cluster/Volumetric SSBO. Per-frame TLAS storage instead uses `VulkanAllocator::AllocateBuffer` + `PushDeletion` (drains N+2) — one VMA allocation per frame per scene. Negligible cost; doesn't disturb the cornerstone primitive. **TLAS build scratch + skinned-refit scratch DO use `GPUTaggedPageAllocator::AllocateLargeTagged`** — they are pure `STORAGE_BUFFER_BIT` consumers, fits the existing backing usage exactly, freed N-2 automatically.

### Instance transform row-major / glm column-major conversion

`VkAccelerationStructureInstanceKHR::transform` is a 3×4 ROW-MAJOR `VkTransformMatrixKHR`. Our `worldMatrix` is `glm::mat4` (column-major 4×4). A naive memcpy ships silently-wrong instance bounds — validation layer doesn't catch it; RT rays miss geometry. Explicit transpose-and-truncate in `TlasBuilder::ToVkTransform`:

```cpp
for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
        out.matrix[r][c] = m[c][r];
```

Documented as a `// CONTRACT:` block per `arch/rendering-pipeline.md`'s cross-pass numerical contracts pattern. Catchable in RenderDoc by inspecting the TLAS instance entry's translation against the entity's known world position.

### Multi-view guard at the build site, not at the pass-add site

`RenderPipeline::Execute` runs once per `RenderView` (typically Scene + Game = 2 per frame). `RtSubsystem::AddTlasBuildPass` is called from `Execute`, so the RG pass gets registered twice per frame. Two options:

1. Guard at the call site in `Execute` — skip the call on subsequent views.
2. Guard inside the pass execute body — pass registers but the body short-circuits via `m_LastBuildFrame`.

Picked (2). Reason: View 2's RG still has the pass slot which keeps the dependency graph consistent (when B.3 lands its consumer in `Execute`, the consumer's `Read` on the TLAS state declares a dep on `TlasBuildPass` — whether or not the pass body actually records cmds). The body's early-return keeps cost at one comparison + assignment. View 2's `GlobalSubsystem::UpdateUBO` reads `m_LastResult.tlas` (the same handle View 1 published) and writes it into View 2's Set 0 binding 6 normally.

### `BoneMatrixBuffer` descriptor binding stageFlags gain COMPUTE_BIT

The bone SSBO was only used by the vertex shader (Set 4 in the main pipeline). Adding `VK_SHADER_STAGE_COMPUTE_BIT` lets the skinning compute reuse the exact same descriptor SET handle that the rendering pipeline binds — no separate descriptor pool, no per-frame rebind dance, no second cycled set. The vertex shader is unaffected; the layout is "compatible" with whatever was bound before.

### `PARTIALLY_BOUND_BIT` on Set 0 binding 6

Boot-time / pre-scene frames: `GlobalSubsystem::UpdateUBO` runs in `Execute` before `AddTlasBuildPass` records anything for the current frame, so the TLAS handle it reads from `RtSubsystem::GetTlas()` is the **previous frame's** value — `VK_NULL_HANDLE` for frame 0. Writing a null AS handle is legal under `PARTIALLY_BOUND` when no shader statically accesses the binding (B.2 has no such shader). Without the flag, validation would refuse the null write even though nothing reads it. The flag is set alongside the existing `UPDATE_AFTER_BIND` on binding 6 only — bindings 0-5 keep just UAB.

---

## Files touched

**Engine (Luth.lib):**

New
- [`VulkanAccelerationStructure.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/VulkanAccelerationStructure.h) — RAII wrapper + static/skinned BLAS factories + Refit (B.2.B/C)
- [`TlasBuilder.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/TlasBuilder.h) — per-frame TLAS build + hash skip + batched skinned-BLAS refit (B.2.D)
- [`SkinningSubsystem.{h,cpp}`](../../../luth/source/luth/renderer/subsystems/SkinningSubsystem.h) — compute pipeline + per-frame dispatch loop (B.2.C/D)

Modified
- [`VulkanBuffer.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/VulkanBuffer.cpp) — AS-build-input flag + upload-fence accessor (B.2.A)
- [`Mesh.{h,cpp}`](../../../luth/source/luth/renderer/resources/Mesh.h) — BLAS ownership + vertex count + isSkinned + out-of-line dtor (B.2.B)
- [`Model.cpp`](../../../luth/source/luth/renderer/resources/Model.cpp) — BLAS build call site (static + skinned routing) (B.2.B/C)
- [`BoneMatrixBuffer.cpp`](../../../luth/source/luth/renderer/resources/BoneMatrixBuffer.cpp) — descriptor binding stageFlags += COMPUTE_BIT (B.2.C)
- [`RenderGraphResources.h`](../../../luth/source/luth/renderer/rendergraph/RenderGraphResources.h) + [`RenderGraph.cpp`](../../../luth/source/luth/renderer/rendergraph/RenderGraph.cpp) — `ResourceState::AccelerationStructure{Build,Read}` + `GetStateInfo` arms (B.2.D)
- [`RtSubsystem.{h,cpp}`](../../../luth/source/luth/renderer/subsystems/RtSubsystem.h) — `AddTlasBuildPass` + `GetTlas` + multi-view guard (B.2.D)
- [`GlobalSubsystem.cpp`](../../../luth/source/luth/renderer/subsystems/GlobalSubsystem.cpp) — Set 0 layout 6→7 bindings + batched TLAS write (B.2.E)
- [`ViewResources.cpp`](../../../luth/source/luth/renderer/ViewResources.cpp) — descriptor pool gains AS-type slot (B.2.E)
- [`RenderPipeline.{h,cpp}`](../../../luth/source/luth/renderer/RenderPipeline.h) — `SkinningSubsystem` registration + `AddTlasBuildPass` call in `Execute` (B.2.C/E)

**Shaders:**
- [`skinning.comp`](../../../luth/assets/shaders/skinning.comp) — per-vertex compute skinning, BDA push-constant input/output, BoneMatrixBuffer set 0 binding 0 (B.2.C)

**Docs:**
- [`arch/rendering-pipeline.md`](../../arch/rendering-pipeline.md) — Set 0 row 6→7 bindings + B.2 description
- [`arch/multi-queue.md`](../../arch/multi-queue.md) — AS-build queue paragraph
- [`epics/rt-renderer.md`](../../epics/rt-renderer.md) — B.2 Progress Tracker row + corrected queue-requirement text

---

## Verification

Build clean (Debug + Release; 18 pre-existing warnings, 0 new). Editor boots; `Vulkan GPU: <name>` (RT-mandatory passes); `RtSubsystem: smoke-test traceRays OK` (B.1 carry-over). `LH_CORE_TRACE("BLAS built ...")` and `LH_CORE_TRACE("Skinned BLAS built ...")` lines appear during scene load.

End-to-end smoke checklist (deferred to user runtime):
1. Load Bhaal Temple — confirm one `BLAS built` log per mesh.
2. RenderDoc capture — `vkCmdBuildAccelerationStructuresKHR` visible on AsyncCompute primary; one TLAS build per frame with `geometryType = INSTANCES_KHR`; Set 0 binding 6 populated with the TLAS device address; instance transforms in the TLAS inspector match entity world positions (catches the row-major-vs-column-major bug).
3. Validation layers clean — zero new VUIDs around VUID-vkCmdBuildAccelerationStructuresKHR-*, VUID-VkWriteDescriptorSetAccelerationStructureKHR-*, VUID-vkDestroyAccelerationStructureKHR-*.
4. Multi-view (Scene + Game panels) — one TLAS build per frame total, not two.
5. 5+ minute steady-state — no `MemoryTracker` growth in `Category::GPU`.
6. Scene load + scene unload + scene reload (5+ cycles) — no GPU memory leak.
7. Frame time within ±5% of v3.0.8 baseline — TLAS build cost (~100-200 µs for ~100 instances on RTX 3080) overlaps with graphics work on AsyncCompute.

If validation surfaces VUIDs around `Vulkan12Features::scalarBlockLayout` for the `skinning.comp` scalar buffer_reference, enable that feature in `VulkanContext::CreateLogicalDevice` alongside `bufferDeviceAddress` — `std430` layout is what we declared so the feature should not be needed, but document it here as the first thing to check.
