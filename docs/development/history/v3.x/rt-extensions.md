# rt-renderer.B.1 — rt-extensions

**Date:** 2026-05-26
**Commits:** 7 (on `feat/rt-extensions`)
**Issue:** [#137](https://github.com/Hekbas/Luth/issues/137)
**Umbrella:** [#127](https://github.com/Hekbas/Luth/issues/127)
**Series:** `rt-renderer`, Phase B opener. Mode A series-coalesced — `Version.h` PATCH bumps `v3.0.7` → `v3.0.8`, tag-only, no Release.

---

## Overview

Phase B of the `rt-renderer` arc opens here. Pure-infrastructure effort modeled on A.1 `bindless-migration`: turn on the four KHR ray-tracing extensions, validate the features they require, load the device-level entry points, and stand up the factory classes for ray-tracing pipelines + shader binding tables. No BLAS/TLAS yet (B.2). No production RT shader yet (B.3). The arc commits to **RT-mandatory** here — devices missing any of the four extensions become ineligible at the picker, not a soft-fallback candidate.

A validation-gated smoke test inside `RtSubsystem::Init` compiles a no-op raygen, builds the pipeline + SBT, records `vkCmdTraceRaysKHR(1,1,1)` via `ImmediateSubmit`, and tears everything down before frame 0. Zero Release cost; B.2 inherits a foundation whose alignment math, fp loading, and create-info chains have already round-tripped through a real driver.

Scope decisions taken during planning (recorded so B.2/B.3 don't relitigate):
- **Set 0 TLAS binding + Set 6 RT-output/SBT layout deferred** to B.2/B.3. Descriptor layouts land with their first writer; B.1 doesn't allocate either. The spec's descriptor-table row gets a footnote pointing at this.
- **Smoke-test included** (gated by `LUTH_ENABLE_VALIDATION`). Without it the SBT/pipeline classes go untested for ≥2 efforts since B.3 may use ray queries (inline) rather than the RT pipeline path.
- **Source-side TAA de-jitter NOT in B.1.** Lands as its own pre-B.3 prep effort (FUTURE.md HIGH item).
- **ShaderStage RT enum + ShaderCompiler stage mappings ship now.** B.1.F fills in the six RT slots that `Shader.h` already documented as "Future". `Shader::Create` factory / `ShaderImporter` / hot-reload paths stay untouched — B.3 audits them with the first production RT shader.

Plan-mode discipline surfaced no architectural deviations: every new mechanism composes with an existing primitive — `VulkanAllocator::AllocateMappedSequentialBuffer` (SBT host-visible buffer), `VulkanContext::PushDeletion` (SBT lifetime), `PipelineCache::Get()` (RT pipeline cache), `VKComputePipeline` (structural template for `VKRayTracingPipeline`), `GTAOSubsystem` (template for `RtSubsystem`), `ShaderCompiler::Compile` (smoke shader path bypassing the half-wired asset layer). No new files in `luth/source/luth/memory/` or `luth/source/luth/jobs/`. No new sync primitives. No `std::mutex` on hot paths.

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| A | **Enable 4 RT extensions + feature probe + RT-mandatory.** Added `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations` to `deviceExtensions`. `DeviceMeetsBaseline` checks all four alongside `VK_KHR_swapchain` — devices without RT become ineligible, so the picker hard-fails at line 230 rather than after `vkCreateDevice`. Probe block extended with `VkPhysicalDeviceAccelerationStructureFeaturesKHR` + `VkPhysicalDeviceRayTracingPipelineFeaturesKHR` + `VkPhysicalDeviceRayQueryFeaturesKHR`, appended at the chain tail (avail11 → AS → RT → RQ). Enable block sets the three RT booleans `VK_TRUE` on matching feature structs at the same chain position. The existing `bufferDeviceAddress = VK_TRUE` (v3.0.0) stays — `acceleration_structure` requires it per Vulkan spec. | `3b7aad8` |
| B | **Load KHR ray-tracing entry points via `vkGetDeviceProcAddr`.** New `VulkanContext::RtFunctions` POD groups 8 PFN_ pointers (vkCreateAccelerationStructureKHR / vkDestroyAccelerationStructureKHR / vkGetAccelerationStructureBuildSizesKHR / vkCmdBuildAccelerationStructuresKHR / vkGetAccelerationStructureDeviceAddressKHR / vkCreateRayTracingPipelinesKHR / vkGetRayTracingShaderGroupHandlesKHR / vkCmdTraceRaysKHR). `LoadRayTracingFunctions()` resolves each via `vkGetDeviceProcAddr`, hard-fails on null (RT-mandatory). Called from `CreateLogicalDevice` right after `vkCreateDevice` succeeds. `GetRtFn()` accessor exposes the struct to future RT subsystems. | `4e36b3a` |
| C | **Query RT physical-device properties.** Cached `VkPhysicalDeviceRayTracingPipelinePropertiesKHR m_RtPipelineProperties` + `VkPhysicalDeviceAccelerationStructurePropertiesKHR m_AsProperties` on `VulkanContext`. Queried via `VkPhysicalDeviceProperties2` + pNext chain at the start of `CreateLogicalDevice` — single site after picker settles (avoiding the two return paths in `PickPhysicalDevice`). INFO log surfaces `shaderGroupHandleSize` / `baseAlignment` / `handleAlignment` / `maxRecursionDepth` / `maxGeometryCount`. SBT-stride math in B.1.E reads from here. | `705890c` |
| D | **`VKRayTracingPipeline` class.** RAII RT pipeline wrapper, RT analog of `VKComputePipeline`. Ctor takes `RayTracingStages` POD (SPIR-V blobs + `VkRayTracingShaderGroupCreateInfoKHR` descriptors), descriptor layouts, push constants, `maxRecursionDepth`. Builds `VkShaderModule`s, `VkPipelineLayout`, `VkRayTracingPipelineCreateInfoKHR`, calls `ctx.GetRtFn().vkCreateRayTracingPipelinesKHR(... PipelineCache::Get() ...)`. Caches `m_GroupHandles` via `vkGetRayTracingShaderGroupHandlesKHR` for the SBT builder. `Bind()` issues `vkCmdBindPipeline(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)`. | `93219df` |
| E | **`RtShaderBindingTable` builder.** Persistent host-visible `VkBuffer` (via `VulkanAllocator::AllocateMappedSequentialBuffer`) with usage `SHADER_BINDING_TABLE_BIT_KHR | SHADER_DEVICE_ADDRESS_BIT | TRANSFER_DST_BIT`. Canonical region order: raygen → miss → hit → callable. Per-handle stride = `alignUp(shaderGroupHandleSize, shaderGroupHandleAlignment)`. Per-region base aligned to `shaderGroupBaseAlignment`. Raygen region's size = stride (per spec — only one raygen invoked per traceRays). `GetRaygen/Miss/Hit/CallableRegion()` accessors return the 4 `VkStridedDeviceAddressRegionKHR` structs ready for `vkCmdTraceRaysKHR`. Dtor defers buffer free via `VulkanContext::PushDeletion`. | `6ea1262` |
| F | **ShaderCompiler RT-stage extension + RtSubsystem + smoke test.** `ShaderStage` enum filled with `Raygen/Miss/ClosestHit/AnyHit/Intersection/Callable` (4-9). `ShaderCompiler::InferStage` recognizes `.rgen/.rmiss/.rchit/.rahit/.rint/.rcall`. `ToShadercKind` maps to `shaderc_glsl_raygen_shader` etc. New `luth/assets/shaders/rt_smoke.rgen` is a no-op raygen (`void main() {}`). New `RtSubsystem` registered as the 9th subsystem on `RenderPipeline`; init last (after Volumetric), shutdown first (before DebugDraw). Under `#if LUTH_ENABLE_VALIDATION`, `RtSubsystem::Init` compiles the smoke shader via `ShaderCompiler::Compile` (bypassing the un-wired `Shader::Create` factory), builds a raygen-only `VKRayTracingPipeline` with empty descriptor layouts + 0 push constants, builds an `RtShaderBindingTable` with `raygenCount = 1`, records `vkCmdTraceRaysKHR(1,1,1)` via `ImmediateSubmit`, logs success, destructs. Release builds: stub `Init` logging idle. | `ea6f5f2` |
| G | **Wrap-up + `Version.h` bump.** This history file. `arch/rendering-pipeline.md` descriptor-table footnote: Set 0 RT additions + Set 6 layout deferred to B.2/B.3 (the spec target shape still describes the end state; B.1 lands the scaffolding only). `rt-renderer.md` Progress Tracker B.1 row → done. **`Version.h` bumped 3.0.7 → 3.0.8.** `--no-ff` merge into `main` + `v3.0.8` tag, no Release (Mode A intermediate). | this commit |

---

## Architectural decisions

### RT-mandatory enforcement at the picker, not after `vkCreateDevice`

`DeviceMeetsBaseline` was extended with the four RT extension names, not as a separate post-pick gate. Why: the picker iterates devices and falls back to a non-discrete-GPU one if the discrete one fails baseline; if RT-mandatory was a post-pick gate, a non-RT discrete GPU + RT-capable integrated GPU machine would crash instead of using the iGPU. Folding RT into the baseline means the iGPU gets picked correctly (or, if neither has RT, both fail at the picker and the `LH_CORE_CRITICAL` message names RT explicitly).

The spec said `RT-mandatory check at device selection`. The natural reading is "at picker time", and the existing baseline-loop structure makes that the cheap option.

### `RtFunctions` as a `VulkanContext` member struct, not a global

Three options were considered:

- Global `static PFN_*` vars in an anonymous namespace inside `VulkanContext.cpp`. Cheap; mirrors the legacy `SetDebugName` lazy-static at line 16-17. Loses const-ref ownership.
- `RtFunctions` POD member on `VulkanContext` with `GetRtFn()` getter. Slightly heavier (24 bytes × 8 pointers on the struct). Gains uniform access pattern + const-ref discipline.
- Singleton class `RtFunctionTable` with its own header. Over-engineered for B.1's scope.

Option 2 won because every future RT consumer (BLAS builder in B.2, RT pipeline binder in B.3, ReSTIR resampler in C.1) needs the same set of pointers, and routing them through `ctx.GetRtFn()` keeps call sites consistent with the existing `ctx.GetGraphicsQueue()` / `ctx.GetAllocator()` style. Cost is trivial.

### SBT is persistent, not per-frame

Per the CLAUDE.md memory cornerstone, per-frame data goes through `GPUTaggedPageAllocator` and persistent data goes through `VKBuffer`+VMA. SBTs are rebuilt only when their RT pipeline rebuilds (rare — only on shader hot-reload or device-reset), not per frame. Persistent path. The SBT is mapped HOST_VISIBLE (via `AllocateMappedSequentialBuffer`) rather than going through a staging upload because typical SBTs are a few hundred bytes — staging adds overhead without perf gain at that size. The flush call (`VulkanAllocator::FlushSlice`) keeps the math correct on non-coherent memory types.

### Smoke test: traceRays(1,1,1) with raygen-only pipeline + empty descriptors

The minimal valid RT dispatch. The raygen does nothing (`void main() {}`), so:

- No descriptor sets needed (empty `layouts` vector).
- No push constants needed (empty `pushConstantRanges` vector).
- No miss / hit / callable shaders needed — those regions of the SBT default-construct (`deviceAddress=0, stride=0, size=0`), which is spec-legal when the corresponding stage isn't invoked.
- 1 ray dispatch — no fan-out, no storage image write, no UAV race.

What this validates end-to-end: extension load order (`DeviceMeetsBaseline` accepts the device), feature struct chaining (validation layer accepts the device-create pNext chain), fp resolution (`vkGetDeviceProcAddr` returns non-null for the 8 entry points), pipeline creation (`vkCreateRayTracingPipelinesKHR` succeeds), handle retrieval (`vkGetRayTracingShaderGroupHandlesKHR` returns N×handleSize bytes), SBT alignment math (region base + handle stride satisfy `shaderGroupBaseAlignment` + `shaderGroupHandleAlignment`), dispatch (`vkCmdTraceRaysKHR` returns), and fence synchronization (`ImmediateSubmit` waits cleanly).

What it does NOT validate: any RT-pipeline-with-actual-shaders path, descriptor-set binding for RT pipelines, push-constant binding for RT pipelines, async-compute submission for `vkCmdTraceRaysKHR`. B.3 picks those up with the first production RT shader.

### `LUTH_ENABLE_VALIDATION` as the smoke-test gate, not `LUTH_BUILD_DEBUG`

`LUTH_BUILD_DEBUG` would gate by build config (Debug only). `LUTH_ENABLE_VALIDATION` derives from `LUTH_BUILD_DEBUG` by default but lets a user force validation on in a Release build for one-off profiling — and in that case the smoke test is a useful check that the Release build's RT path still works end-to-end. The cost is one `ImmediateSubmit` at startup (<1 ms), so the override case is fine to pay.

### ShaderStage RT enum filled now, asset-pipeline integration deferred

`Shader.h` already documented `Raygen` as a "Future" slot. Filling in 6 RT enum values + 6 lines per `InferStage`/`ToShadercKind` switch is small (~25 LOC across 3 sites), so doing it now means B.3's first RT-shader-load path doesn't have to revisit these files. What's NOT done: `Shader::Create` factory (probably returns null for `ShaderStage::Raygen` today), `ShaderImporter` (will it serialize a `.rgen` artifact correctly?), `ShaderWatcher` hot-reload for RT stages. The smoke test bypasses all of these by calling `ShaderCompiler::Compile` directly — the lowest-level path that doesn't touch the asset layer. B.3 audits the asset layer when the production RT shadow shader needs to be assignable to a `Material`/pipeline through the normal channels.

### Smoke test bypasses `ShaderLibrary::LoadEngine`

`ShaderLibrary::LoadEngine` would go through `Shader::Create` → `VulkanShader` ctor → cache in `s_Shaders` map → potentially fire ShaderWatcher hooks. Any of those could trip on a `ShaderStage::Raygen` they don't know how to handle. The smoke test is supposed to validate the **RT plumbing**, not the asset layer. Calling `ShaderCompiler::Compile` directly isolates the validation to the part B.1 actually changes. If a future RT shader breaks the asset layer, that's a B.3 bug, not a B.1 false positive.

---

## Files touched

**Engine (Luth.lib):**
- [`VulkanContext.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/VulkanContext.cpp) — extension list, baseline check, feature probe + enable chain extensions, `RtFunctions` POD, `LoadRayTracingFunctions`, RT property caches + query
- [`VulkanRayTracingPipeline.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/VulkanRayTracingPipeline.h) — RT pipeline factory class (new)
- [`RtShaderBindingTable.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/RtShaderBindingTable.h) — SBT region builder (new)
- [`Shader.h`](../../../luth/source/luth/renderer/shader/Shader.h) — `ShaderStage` enum + 6 RT entries
- [`ShaderCompiler.cpp`](../../../luth/source/luth/renderer/shader/ShaderCompiler.cpp) — `InferStage` + `ToShadercKind` RT mappings
- [`RtSubsystem.{h,cpp}`](../../../luth/source/luth/renderer/subsystems/RtSubsystem.h) — subsystem + smoke test (new)
- [`RenderPipeline.{h,cpp}`](../../../luth/source/luth/renderer/RenderPipeline.h) — `m_Rt` member + `GetRt()` + Init/Shutdown wiring

**Shaders:**
- [`rt_smoke.rgen`](../../../luth/assets/shaders/rt_smoke.rgen) — no-op raygen for the validation smoke test (new)

**Docs:**
- [`arch/rendering-pipeline.md`](../arch/rendering-pipeline.md) — descriptor-table Set 0/Set 6 deferral footnote
- [`epics/rt-renderer.md`](../epics/rt-renderer.md) — B.1 Progress Tracker row

---

## Verification

All sub-tasks: C++ builds clean (Debug + Release; zero new warnings). Smoke test runs during editor boot in Debug builds.

End-to-end smoke checklist:
1. Editor launches; `Vulkan GPU: <name>` (device pick passed RT-mandatory).
2. `RT: shaderGroupHandleSize=N baseAlignment=M handleAlignment=K maxRecursionDepth=X maxGeometryCount=Y` log line appears (vendor-dependent values — NVIDIA Ampere/Ada typical: 32/64/32/31/2^24).
3. `RtSubsystem: smoke-test traceRays OK` log line appears (Debug builds).
4. Release build boot logs `RtSubsystem: idle (Release build — smoke test disabled)`.
5. Validation layers clean — no new VUIDs around `VkDeviceCreateInfo-pNext` (feature struct chains), `vkCmdTraceRaysKHR` (SBT alignment), or `VkRayTracingPipelineCreateInfoKHR` (group index / recursion depth).
6. Frame Debugger: graph byte-identical to v3.0.7 (no new passes, no descriptor changes). Confirms B.1 is pure infrastructure.
7. RT-mandatory negative test: strip ≥1 RT extension from `deviceExtensions` and rebuild; editor must hard-fail at the picker's `LH_CORE_CRITICAL("No Vulkan device meets baseline ...")` log. Restore extensions and confirm boot resumes.
8. No regression in the existing test scene — visual + frame time identical to v3.0.7.
