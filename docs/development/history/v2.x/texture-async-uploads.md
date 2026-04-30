# v2.8.14 — texture-async-uploads

**Date:** 2026-04-30
**Commits:** 5 (on `feat/texture-async-uploads`) + wrap-up
**Issue:** [#107](https://github.com/Hekbas/Luth/issues/107)

---

## Overview

Finishes the texture half of `vulkan-polish` S4, deferred mid-execution because mip-chain submit + deferred-bindless-registration expanded scope from M to L. Three composable changes inside `UploadContext` plus a single VKTexture wiring change retire the per-texture `vkWaitForFences UINT64_MAX` stall that `VulkanContext::ImmediateSubmit` enforced for every texture upload, replace the F3 `vulkan-polish` stopgap that serialized back-to-back uploads on a single shared cmd buffer, and decouple bindless slot registration from upload completion. Composition-only — no new architectural primitives. Tag-only release per the v2.8.5 internal-architecture policy.

The new `UploadContext::UploadImageMipped` records pre-barrier across all mips → mip-0 staging copy → `vkCmdBlitImage` chain with per-mip transitions → final SHADER_READ_ONLY across the range, all in one cmd-buffer submit. Stays on the graphics queue forever (BLIT_DST_BIT not on transfer family, even after `async-compute-queue` v2.9.2's split). The single `m_CommandBuffer` was replaced with a 4-slot ring tracked by per-slot fence values; the F3 stopgap pre-reset wait was dropped from all three Upload methods, so back-to-back uploads truly overlap on the GPU. The deferred-bindless-registration pump composes with `AssetManager::s_UploadQueue`'s main-thread tick — `VKTexture` ctor pushes `{outIndex, view, sampler, fenceValue}`, the pump checks `IsComplete(fence)` per frame and writes the assigned slot through `outIndex` once the upload retires. Until then `m_BindlessIndex` stays at `INVALID_BINDLESS_SLOT` (the v2.8.13 sentinel) and `Material::BindlessOrNull` keeps materials sampling reserved white slot 0; `MaterialSystem`'s already-unconditional per-frame re-encode picks up the real index automatically once the pump binds it. `~VKTexture` calls `CancelPendingBind(m_ImageView)` before image teardown to prevent the pump from dereferencing freed handles or writing through a stale `outIndex` pointer.

---

## Sub-tasks

| # | Slug | Commit | Notes |
|---|---|---|---|
| S1 | `upload-image-mipped` | `069d782` | New `UploadContext::UploadImageMipped(data, size, image, w, h, mipLevels, arrayLayers)` records the full mip-chain in one cmd-buffer submit. Single-mip path supported (handles `mipLevels==1` as a transition-only fast path so callers don't have to branch). Existing single-mip `UploadImage` (caller-supplied `VkBufferImageCopy`) retained — survives for `async-compute-queue` v2.9.2's transfer-family path. **Smoke deferred to S3** — S1 added the API but no callers; the plan agent's "smoke = 2K drag-drop" marker assumed callsite migration in the same commit. Documented under Deviations below. |
| S2 | `cmd-buffer-ring` | `84d4f5e` | Single `m_CommandBuffer` → `std::array<VkCommandBuffer, 4> m_CmdRing` + `m_RingFenceValues` + round-robin `m_SubmitIndex`. New `BeginRingSlot()` / `RecordRingSlotFence(fence)` helpers consolidate the slot acquire/wait/release dance across `UploadBuffer`, `UploadImage`, and `UploadImageMipped`. The F3 stopgap pre-reset `m_UploadTimeline.Wait(m_CurrentValue)` (added in vulkan-polish commit `60d5b05`) is dropped — slot reuse waits only when that slot's prior fence hasn't retired, which is rare in practice. RING_SIZE=4 chosen so the 64 MB staging ring typically saturates first; submission-driven, independent of `MAX_FRAMES_IN_FLIGHT`. **Smoke (Bistro):** 5818 `UploadBuffer` zones in Tracy with no validation errors — uploads now genuinely overlap on the GPU. |
| S3 | `vktexture-mipped` | `85a1f6a` | Strip the ~100-line inline `ImmediateSubmit` lambda from `VKTexture::CreateImage` data path (`VulkanTexture.cpp:217-327`) including its local staging buffer alloc/map/copy/free. Replace with a single `UploadContext::UploadImageMipped` call; staging now flows through UploadContext's 64 MB ring. New members `m_LastUploadFence`/`m_DidAsyncUpload` introduced for S5's deferred-bind. Synchronous `WaitForUpload(m_LastUploadFence)` retained at this stage so `CreateViewAndSampler` line 453 still calls `BindTexture` against an already-uploaded image — same end-to-end semantics, leaner path. **Smoke (Bistro):** 250 `VulkanContext::ImmediateSubmit` zones replaced by 250 `UploadContext::UploadImageMipped` zones — direct 1:1 substitution, no validation errors. RT/depth `ImmediateSubmit` (line 347, layout transition only — never carries pixel data) and the 4 sync init/control-flow sites untouched: `BindlessDescriptorSet::CreateNullTexture`, all `IBLPrecompute` GPU work, `PickingSystem` readback, `FrameDebuggerContext` capture. |
| S4 | `pending-bind-queue` | `81ed6fc` | New `PendingBind` struct + `m_PendingBinds` vector under existing `m_Lock`. APIs: `PushPendingBind(outIndex, view, sampler, fenceValue)` enqueue, `DrainPendingBinds()` poll-completed-and-pop with single `GetValue()` query and reverse swap-and-pop iteration, `CancelPendingBind(view)` linear scan + erase by view-handle. View handle is the natural cancel key — one-to-one with VKTexture, immutable across its lifetime, never reused across instances. Lock order `UploadContext::m_Lock → BindlessDescriptorSet::m_Lock` consistent with existing nested call paths. No callers in this commit; S5 wires VKTexture push/cancel + AssetManager drain. |
| S5 | `deferred-bind-wiring` | `f2c3a19` | The closing wiring change. `~VKTexture` calls `CancelPendingBind(m_ImageView)` before the existing `UnbindTexture(m_BindlessIndex)` and the deferred image/view/sampler `PushDeletion` — pump cannot deref freed handles. `CreateImage` drops the synchronous `WaitForUpload` added in S3; upload submit now truly returns immediately. `CreateViewAndSampler` line 453 routes through `PushPendingBind` when `m_DidAsyncUpload` is true; sync-path color RTs (`data==nullptr`) keep immediate `BindTexture` registration. `AssetManager::Update` calls `UploadContext::Get().DrainPendingBinds()` every frame — outside the `s_UploadMutex` lock_guard scope and unconditional on `s_UploadQueue.empty()`, since idle frames must still tick the pump for entries pushed earlier. `Closes #107`. |
| S6 | wrap-up | (this commit) | Version bump 2.8.13 → 2.8.14. New BACKLOG entry for `texture-async-uploads` (Shipped status); new BACKLOG stub for `procedural-sky` (post-Jolt, default no-HDR experience). ROADMAP Completed table appended with `vulkan-polish` (v2.8.13) and `texture-async-uploads` (v2.8.14) rows; Planned Epics table updated. CLAUDE.md "Active series" / current version refreshed. |

---

## Architectural alignment

Every sub-task composes with an existing primitive named in `arch/`. Notable references:

- **S1** composes with `UploadContext` (VMA-backed 64 MB staging ring, `TimelineSemaphore`-tracked, blessed by `arch/profiling.md:62` and called out by `arch/asset-pipeline.md`). New `UploadImageMipped` is a sibling to the existing `UploadImage`/`UploadBuffer`, not a replacement — single-mip path stays for `async-compute-queue` v2.9.2's transfer-family migration.
- **S2** composes with `VulkanBackend::m_PrimaryCommandBuffers` (the canonical "ring of cmd buffers tracked by fence" pattern at `MAX_FRAMES_IN_FLIGHT=3` per `arch/frame-pipeline.md`). UploadContext's ring follows the same indexed-by-modulo pattern but submission-driven (size 4, independent of frames-in-flight) — uploads aren't frame-bounded. `CommandAllocatorPool` rejected during plan-mode primitive inventory: designed for fiber acquire/release with frame-boundary reset, wrong lifetime for asset-load-time uploads.
- **S3** composes with `UploadContext::UploadImageMipped` (S1) + `VulkanContext::ImmediateSubmit` (kept for the 5 init/control-flow sync sites that genuinely need synchronous semantics). No new sync primitive. **V3 not triggered** — `VKTexture::CreateImage` runs on the main thread (`arch/asset-pipeline.md:87`: GPU resource creation is main-thread-only), no fiber yield between record and submit.
- **S4** composes with `VulkanContext::m_DeletionQueues` *shape* (vector + lock + drain — see `VulkanContext.cpp:463-491`) but with poll-each-frame predicate (`UploadContext::IsComplete(fence)`) instead of frame-N-2 retirement. Lives inside `UploadContext` because the fence belongs there; a separate file would be a third drain pump for no architectural reason.
- **S5** composes with `MaterialSystem::Update`'s already-unconditional per-frame re-encode (`MaterialSystem.cpp:102` — no "if dirty" check, comment line 101: "Refresh GPU data each frame to pick up newly-loaded bindless texture indices"). This was the **load-bearing answer** from Phase-1 inventory: it means the deferred-bind pump needs **no explicit material re-dirty propagation**. The unconditional re-encode is the recovery mechanism. `Material::BindlessOrNull` (`Material.cpp:21`, vulkan-polish S2) handles the sentinel coercion when `m_BindlessIndex == INVALID_BINDLESS_SLOT` during the brief gap between ctor and pump fire.

**Cornerstones honored:**
- **#1 Memory** — UploadContext is VMA staging, orthogonal to `GPUTaggedPageAllocator` (per `arch/memory.md` and `vulkan-polish.md:40`). `std::mutex` retained on `UploadContext::m_Lock` — asset-load-time path, not hot, cornerstone #1's "no `std::mutex` on hot paths" carve-out explicit.
- **#2 Job system** — V3 (cmd-buffer affinity) preserved: all UploadContext record/submit on main thread. V1 (no yield under spinlock) not relevant — no spinlock involved.
- **#5 Vulkan 1.3** — Dynamic Rendering, Timeline Semaphores. No legacy passes. `vkCmdBlitImage` on graphics queue (BLIT_DST_BIT requirement).

---

## Deviations from approved plan

### S1 smoke-gate deferred to S3

The approved plan listed S1 (API addition) with a smoke marker of `Y` ("drag-drop a 2K texture; mips 4-6 visible at distance"). Reading the `Files` column at execution time confirmed the structural intent: `UploadContext.{h,cpp}` only — no callsite migration. With no callers, the texture drag-drop path still flowed through the old inline `ImmediateSubmit` lambda; the smoke test would have exercised the OLD path, not the new API. Decision (recorded in spec mid-execution): treat S1 like S4 (paired-smoke-at-consumer pattern), with the actual mip-chain validation happening at S3 when `VKTexture::CreateImage` migrates. The plan agent's structural design was correct; only the smoke marker was misaligned.

### Drain placement: before `s_UploadMutex` lock instead of after `s_UploadQueue.clear()`

The approved plan placed `DrainPendingBinds()` after `s_UploadQueue.clear()` inside the lock_guard scope. Implementation moved it outside the lock_guard (drain doesn't touch `s_UploadQueue` or `s_LoadingAssets`, no need to hold an unrelated lock during the descriptor write). Functionally equivalent — the one-frame minimum latency for newly-finalized textures is the same regardless of drain-vs-finalize ordering within `Update` (upload fence rarely retires same-frame as submit). The new placement also fixes a latent bug in the original early-return (`if (s_UploadQueue.empty()) return;`) which would have skipped the drain entirely on idle frames.

---

## Smoke-test results

All blocking gates clean. Bistro scene used as the heavy-load reference.

| Gate | Test | Result |
|---|---|---|
| Post-S2 | Bistro cold-load | 5818 `UploadContext::UploadBuffer` zones in Tracy, no validation errors. Buffer uploads now overlap on GPU via 4-slot ring. |
| Post-S3 | Bistro cold-load | 250 `UploadContext::UploadImageMipped` zones replacing the 250 prior `VulkanContext::ImmediateSubmit` zones — direct 1:1 substitution, no validation errors, all textures render correctly. |
| Post-S5 | Bistro cold-load + project reopen cycle | No validation errors. White-fallback frame compressed below visible threshold (see "Known limitations" below). Force-evict + reload (project close/reopen) shows clean re-load with no orphan bindless slots. |

The "white-fallback frame" expected behavior — textures briefly sampling the reserved slot 0 between ctor return and pump fire — does happen as designed but is shorter than one render frame in practice. By the time the editor renders the first frame after `AssetManager::Update` returns, most of the 250 GPU uploads have already retired (they ran in parallel via the ring during Update). The pump in the very next frame binds nearly all of them in one tick. Visible white-fallback would require streaming `s_UploadQueue` finalize across multiple frames — see "Known limitations."

---

## Known limitations

The deferred-bind pump only defers the **descriptor write** (`vkUpdateDescriptorSets`, microseconds). `AssetManager::Update` itself still does, sequentially on the main thread for each of N textures:

- `memcpy` of mip-0 pixel data into the 64 MB staging ring (largest per-texture cost).
- cmd buffer record + `vkQueueSubmit` (fast but adds up).
- staging-ring overflow waits when 12+ textures' worth of mips don't fit.

For Bistro this is "fast but not invisible" — perceptibly faster than pre-S3 (no per-texture `vkWaitForFences UINT64_MAX` stall — that was the dominant cost; 250 textures × ~5-50ms per-fence-wait = several seconds of frame stutter), but still all-at-once on one thread. What this epic delivered:

- **Per-texture latency**: per-texture GPU stall → submit-and-go.
- **GPU upload parallelism**: serial on graphics queue → up to 4 concurrent via cmd-buffer ring.
- **Bindless decoupling**: descriptor write gated on fence retirement, not upload submit.

What this epic did **not** deliver:

- Streaming `s_UploadQueue` finalize across frames (drain N entries per tick instead of all-at-once). Listed in [`BACKLOG.md` "Future" → "Asset Streaming"](../../BACKLOG.md). Separate epic, M-L effort, depends on a per-tick budget mechanism that doesn't exist yet.

---

## Procedural-sky stub added (BACKLOG)

Per discussion during the v2.8.13 IBL-skip-pre-project fix and confirmed in the `texture-async-uploads` kickoff, this wrap-up adds a `procedural-sky` BACKLOG stub for the post-Jolt scheduling slot. Hosek-Wilkie or Preetham analytical sky as the default no-HDR experience, replacing the current dummy-cubemap fallback. M effort, depends on `jolt-physics` for scheduling only (no technical dependency).

---

## Build verification

Solution unchanged; rebuilt Debug x64 after each sub-task. No new warnings. Pre-existing C4244 (`Editor.cpp:402` chrono cast), C4267 (`size_t→uint32_t` in `WinWindow.cpp:221`, `Model.cpp:42,57,68`, and stdlib `xutility` template instantiations), C4996 (`getenv` in `ProjectLauncher.cpp`, `strncpy` in `InspectorPanel.cpp:79`), and LNK4006 NULL_IMPORT_DESCRIPTOR (env, shaderc/vulkan/dbghelp lib stub conflict) all unchanged from pre-epic baseline.

**Smoke tests passed** on Bistro for S2 (5818 UploadBuffer overlap), S3 (250 UploadImageMipped 1:1 substitution), and S5 (deferred-bind cycle clean across cold-load + project reopen). Validation runs clean across all three.
