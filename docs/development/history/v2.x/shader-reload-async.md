# v2.8.12 — shader-reload-async

**Date:** 2026-04-29
**Commits:** 3 (on `feat/shader-reload-async`)
**Issue:** [#105](https://github.com/Hekbas/Luth/issues/105)

---

## Overview

Drops the per-save `vkDeviceWaitIdle` from the shader hot-reload path. The reload callback now builds new pipelines first, then pushes the old `VKPipeline`/`VKComputePipeline` raw pointers to `VulkanContext::PushDeletion` — the per-frame deletion ring (`Luth::SpinLock`-protected since v2.8.7's `vulkan-correctness`). Drained `MAX_FRAMES_IN_FLIGHT` frames later in `AcquireImage`, by which point the GPU has retired any command buffer that bound the old pipeline.

`VulkanShader::Reload` drops its own redundant `vkDeviceWaitIdle`. The `VkShaderModule` is consumed at pipeline-create time per the Vulkan spec — previously-built pipelines hold no reference to the module handle, so destroying the old module immediately is safe (in-flight pipelines are unaffected; their destruction is what's deferred).

`PipelineManager` gains `DeferredClear()` and `DeferredInvalidateShader()` for the cached PBR variants. The synchronous `Clear()`/`InvalidateShader()` stay in place for shutdown (where `vkDeviceWaitIdle` already ran upstream in `VulkanBackend::Shutdown`).

`m_ShaderWatcher.Poll()` moves from per-`Execute` (per-view) to once-per-frame in `RenderingSystem::Update` prologue. Cuts the polled-no-changes overhead in half when both Scene and Game viewports are open.

Tag-only release per the v2.8.5 internal-architecture policy.

---

## Why deferred destroy is the right shape

`VulkanContext::PushDeletion` was made V1-correct in v2.8.7 (`m_DeletionLock` `std::mutex` → `Luth::SpinLock`, sub-task H of that epic). That made it safe to push from any thread under <100 cycles. The shader reload callback runs on the render-stage fiber (now that Poll moved to `RenderingSystem::Update`); pushing under SpinLock is fine.

The drain happens in `VulkanBackend::AcquireImage` for frame N+3 (when `m_FrameTimeline.Wait(N - MAX_FRAMES_IN_FLIGHT + 1)` returns), so old pipelines outlive their last in-flight binding by at least 3 frames. No race possible.

This is the same shape `gpu-tagged-heap`'s `AllocateLargeTagged` uses for one-shot dedicated VkBuffer destruction. Same primitive, same lifetime model.

---

## Sub-tasks

| # | Slug | Commit | Notes |
|---|---|---|---|
| A | Move Poll to once-per-frame | `908f6d5` | `m_ShaderWatcher.Poll()` from `RenderPipeline::Execute` (per-view) to `RenderingSystem::Update` prologue. Behavior identical; one fewer mutex acquire + queue check on the steady-state no-changes path |
| B | Defer pipeline destroy | `0521fd4` | `vkDeviceWaitIdle` dropped from both reload sites. `deferGfx`/`deferComp` lambdas at the top of the callback push old `unique_ptr<VK[Compute]Pipeline>` raw pointers into `PushDeletion`. `PipelineManager::DeferredClear()` and `DeferredInvalidateShader()` added for the PBR cache (12 cached variants would otherwise need a separate defer loop) |
| C | Wrap-up | (this commit) | Version bump 2.8.11 → 2.8.12. ROADMAP/BACKLOG updates. DescriptorAllocator note resolved as stale (IBLPrecompute uses it init-only) |

---

## Lambda capture pattern

`std::function` requires CopyConstructible captures, so `unique_ptr` (move-only) doesn't fit. Pattern:

```cpp
auto deferGfx = [](std::unique_ptr<VKPipeline>& p) {
    if (auto* raw = p.release(); raw)
        VulkanContext::Get().PushDeletion([raw]() { delete raw; });
};
```

`release()` transfers ownership to a raw pointer; the lambda captures the raw by value (CopyConstructible); when the deletion queue invokes it, `delete raw` fires `~VKPipeline` (vkDestroyPipeline + vkDestroyPipelineLayout).

---

## DescriptorAllocator (stale BACKLOG note resolved)

The original BACKLOG entry for this epic listed `DescriptorAllocator` as having "no callers, accumulates unbounded — wire `Reset()` per-frame OR delete the class". Re-checked under this epic:

- **Has callers**: 4 `VulkanContext::Get().GetDescriptorAllocator().Allocate(...)` sites in `IBLPrecompute.cpp`
- **Init-only**: IBL precompute runs once at scene load; descriptor sets persist for the lifetime of those bindings
- **No unbounded growth**: pool is created once at first `Allocate`, sized for ~1000 sets. IBL needs ~10-20. Pool fits in one block; no `OUT_OF_POOL_MEMORY` retry path ever fires in practice

Note resolved as stale; class kept as-is. If a future epic adds per-frame descriptor-set churn through this allocator, revisit.

---

## Build verification

Solution unchanged; rebuilt Debug x64 after each sub-task. No new warnings (the C4267 noise from `xutility` was already present pre-epic via `std::erase_if` in the synchronous `InvalidateShader`).

**Runtime smoke (recommended next):** edit `pbr.frag` mid-run with editor open. Expected: pipelines rebuild without `vkDeviceWaitIdle` stutter; validation clean; ProfilerPanel GPU-memory does not grow unboundedly (deletion queue picks up old pipelines after MAX_FRAMES_IN_FLIGHT frames).
