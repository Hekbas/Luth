# v2.8.13 — vulkan-polish

**Date:** 2026-04-29
**Commits:** 7 (on `feat/vulkan-polish`)
**Issue:** [#106](https://github.com/Hekbas/Luth/issues/106)

---

## Overview

Final foundation-stabilization renderer epic before `jolt-physics` (v2.9.0). Six tier-2/3 cleanups + housekeeping that tighten the cost model for forward+ and gpu-particles without changing observable rendering output. Composition-only — no new memory or sync mechanisms.

The validation messenger is now pNext-chained into `VkInstanceCreateInfo` so `vkCreateInstance` and `vkDestroyInstance` failures get reported instead of going silent. The bindless free-list switches from `std::deque<u32>` to `std::vector<u32>` LIFO, and the long-running overload of slot 0 (used as both the reserved null-texture and a "not registered" sentinel for depth/cubemap/`R32_Uint` textures) is broken: a real `INVALID_BINDLESS_SLOT = UINT32_MAX` constant covers the latter, with a `BindlessOrNull` coercion at every SSBO write site so the sentinel never reaches a shader. `RenderResourceCache` swaps its linear vector scan for `unordered_multimap<u64, PooledResource>` keyed on `(width, height, format, usage)` (with `TextureDesc::usage` defaulting to `0` = "infer from format" for backwards compat), and trims `k_StaleFrameThreshold` from 10000 to 30 frames.

Buffer uploads (`VKVertexBuffer::SetData`, `VKIndexBuffer` ctor) now route through `UploadContext::UploadBuffer` — caller does not wait on the fence, since graphics-queue submission order ensures any draw consuming the buffer (always submitted later in the frame) implicitly observes the upload. The texture half of S4 was deferred mid-execution after reading `UploadContext::UploadImage` revealed it handles only single-mip transitions; multi-mip texture-async needs both a UploadContext API extension and a deferred-bind pump, which is L-shaped work not warranted within an M-effort epic. Captured as a follow-up below. Editor-tunable values (selection outline width/color/occluded-alpha + 7 grid params) are now plumbed through `EditorSettings` → `EditorViewportState` → `CameraParams` → `OutlinePass`/`GridPass`, mirroring the existing `iblIntensity` flow; `RenderingSystem::SetOutlineColor` and the `m_OutlineColor` member are removed since the value flows through `m_CameraParams`. The vestigial `DescriptorAllocator` class — used only by `IBLPrecompute` — is gone; IBL owns a local 8-set descriptor pool created at the start of `Precompute` and destroyed at the end (matching the direct-pool pattern from `GPUObjectBuffers.cpp`).

Tag-only release per the v2.8.5 internal-architecture policy.

---

## Sub-tasks

| # | Slug | Commit | Notes |
|---|---|---|---|
| S1 | `validation-pnext` | `d477d78` | Factor `PopulateDebugMessengerCreateInfo` helper; chain a temporary messenger via `VkInstanceCreateInfo.pNext` so instance create/destroy failures are caught. The chained messenger is consumed by `vkCreateInstance` and not retained — lifetime ends when the call returns, exactly when needed. Persistent messenger from `SetupDebugMessenger` covers steady state. |
| S2 | `bindless-sentinel` | `6cfc290` | `INVALID_BINDLESS_SLOT = UINT32_MAX` (`BindlessDescriptorSet` public). `m_FreeIndices` switched from `std::deque<u32>` to `std::vector<u32>` LIFO (`pop_back`/`push_back`); init pushes `MAX-1..1` so allocations come back in ascending order (matches RenderDoc reading habits). `BindTexture` returns the sentinel on overflow (was `0`, would alias the reserved null slot); `UnbindTexture` early-returns on sentinel and on slot 0. `VKTexture::m_BindlessIndex` defaults to the sentinel; depth/cubemap/`R32_Uint` paths leave it at the sentinel. **GPU-side coercion (load-bearing):** `Material::UpdateGPUData::GetIndex` wraps `tex->GetBindlessIndex()` with `BindlessOrNull(...)` so `UINT32_MAX` never reaches an SSBO field — Set 1 has 16384 slots, sampling at `UINT32_MAX` is UB even with `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` (partial-bind permits unbound slots, not out-of-range indices). |
| S3 | `cache-hash-key` | `10056e6` | `RG::TextureDesc` gains `VkImageUsageFlags usage = 0` (0 = "infer from format" via local `ResolveUsage` helper, preserving existing call sites; future compute-storage transients pass `STORAGE`). `RenderResourceCache::m_Pool` switched from `std::vector<PooledResource>` (linear scan) to `std::unordered_multimap<u64, PooledResource>` keyed on `hash_combine(w, h, format, usage)`; final `DescMatches` equality check guards rare hash collisions producing wrong-tuple matches. `k_StaleFrameThreshold` 10000 → 30 frames (~0.5s @ 60Hz); the prior value was effectively "never evict" and let viewport-resize churn accumulate stale entries. |
| S4 | `async-uploads` (buffers only) | `25765e4` | `VKVertexBuffer::SetData` and `VKIndexBuffer` ctor drop their per-call staging buffer + `ImmediateSubmit`; one-line `UploadContext::Get().UploadBuffer(data, size, dst, 0)`. Caller does not wait on the fence — same-queue submission order makes the sync implicit. Today `UploadContext.cpp:51` runs on the graphics queue (despite the field name `m_TransferQueue`); `async-compute-queue` v2.9.2 will split to a transfer-only family with no S4 API change. **Texture half deferred** — see "Deferred follow-ups" below. |
| S5 | `editor-settings-bridge` | `12a641a` | `EditorSettings` gains 10 fields (1 outline color + 2 outline numbers + 3 grid colors + 4 grid floats); JSON Vec4 helpers (`LoadVec4` / `ToJson`) added. `EditorViewportState` mirrors them; `LuthienEditorHooks::GetViewportState` populator copies from `Editor::GetSettings()` each frame. `CameraParams` (the canonical per-frame editor-state struct that already carries `iblIntensity`/`skyboxIntensity`) gets the same fields; `App.cpp` fills them into the `cp` passed to `RenderingSystem::SetCameraParams`. `OutlinePass` / `GridPass` read `m_System.m_CameraParams` instead of shader-baked literals — engine includes zero `luthien/`, cornerstone #6 honored, same flow as `iblIntensity`. `RenderingSystem::m_OutlineColor` + `SetOutlineColor` removed (now via `CameraParams`); `ScenePanel` drops the redundant per-frame setter call. `RenderPanel` exposes both blocks under "Selection Outline" and "Editor Grid" collapsing headers; persisted via `EditorSettings.json`. |
| S6 | `delete-descriptor-allocator` | `64ceaf4` | `IBLPrecompute` owns a local 8-set `VkDescriptorPool` created at the start of the HDR-loaded branch and destroyed at the end (8 sets = equirect + irradiance + 5 prefilter mips + BRDF LUT; small `AllocateIBLSet` helper mirrors the previous `DescriptorAllocator::Allocate` ergonomics). 4 alloc sites migrated. `DescriptorAllocator` class + `m_DescriptorAllocator` member + `Init`/`Shutdown` calls + `GetDescriptorAllocator` accessor all gone — IBL was the only consumer and the abstraction added no value over the direct-pool path already used by `GPUObjectBuffers.cpp`. Net deletion 77 LOC. |
| S7 | wrap-up | (this commit) | Version bump 2.8.12 → 2.8.13. BACKLOG: duplicate `---` divider removed near the vulkan-polish heading; bullet-3 framing rewritten to drop the stale "collides with slot 0" claim; entry marked Shipped with link to this file. CLAUDE.md "Active series" updated. `Closes #106`. |

---

## Architectural alignment

Every sub-task composes with an existing primitive named in `arch/`. No new mechanisms. Notable references:

- **S2** composes with `BindlessDescriptorSet` (`arch/rendering-pipeline.md` Set 1, 16384 slots, `UPDATE_AFTER_BIND` + `PARTIALLY_BOUND_BIT`). `std::vector<u32>` LIFO is the same primitive class as the previous `std::deque<u32>`, lighter cache footprint. `std::mutex` retained on `BindlessDescriptorSet::m_Lock` — asset-load-time only, not per-frame, so cornerstone #1's "no `std::mutex` on hot paths" sub-point doesn't force a `Luth::SpinLock` migration here.
- **S4** composes with the existing `UploadContext` (VMA-backed 64MB staging ring, `TimelineSemaphore`-tracked, blessed by `arch/profiling.md:62` and called out by `arch/asset-pipeline.md`). No new sync primitive. **V3 not triggered** — `VKVertexBuffer::SetData` and `VKIndexBuffer` ctor run on the main thread (`arch/asset-pipeline.md:87`: GPU resource creation is main-thread-only), no fiber yield between record and submit. **Cornerstone #1 not violated** — UploadContext is a VMA staging buffer, orthogonal to `GPUTaggedPageAllocator` (which covers per-frame SSBOs only).
- **S5** composes with `EditorViewportState` + `IEditorHooks::GetViewportState` (`arch/editor.md:11-24`). Engine reads its own struct, not `luthien/`. Cornerstone #6 honored. Same flow as the existing `iblIntensity`/`skyboxIntensity` plumbing in `LuthienEditorHooks::GetViewportState`.
- **S6** composes with the direct `vkCreateDescriptorPool` pattern already used by `GPUObjectBuffers.cpp` — the engine's canonical approach for one-shot descriptor needs. The deleted `DescriptorAllocator` class added no value over this for IBL's 8-set lifetime.

---

## Slot-0 reframing (deviation from BACKLOG sketch)

The BACKLOG entry for bullet 3 said the bindless free-list "collides with the null-texture's slot 0." Verified during Phase 1: the previous `BindlessDescriptorSet::Init` (`VulkanDescriptors.cpp:156-158` pre-S2) already explicitly popped slot 0 from the free-list before any `BindTexture` could run, so no real collision was possible. The genuine overload was *semantic*: `VKTexture::m_BindlessIndex == 0` doubled as both "this is the reserved null-texture slot" *and* "this texture is not registered in the bindless set" (depth, cubemap, `R32_Uint` paths). The dtor guard `if (m_BindlessIndex != 0)` only worked by coincidence — slot 0 stayed reserved, so no real bindless texture ever had index 0.

S2's `INVALID_BINDLESS_SLOT = UINT32_MAX` disambiguates the two: slot 0 keeps its job (the reserved 1×1 white fallback), and the C++ "not registered" signal moves to a value that can never be a real slot. The dtor guard, the depth/cubemap/`R32_Uint` early-returns, and the SSBO-write coercion (`BindlessOrNull`) all key on the new sentinel.

---

## S4 scoped to buffers only (deviation from approved plan)

The approved plan budgeted S4 as M effort, splitting `VKTexture::CreateImage` into "mip-0 staging via `UploadContext` / mip-chain blit via `ImmediateSubmit`." Reading `UploadContext::UploadImage` (UploadContext.cpp:218-297) at execution time revealed it transitions UNDEFINED→TRANSFER_DST→SHADER_READ_ONLY internally **for single-mip images only**. Multi-mip texture uploads (the asset-loader's common case) would split across two graphics-queue submissions, with the second (`ImmediateSubmit` for the blit) still synchronous — same total wall time on the calling thread, no async benefit. The CPU-side gain only materializes with **deferred bindless registration**: ctor returns immediately, a main-thread pump checks `IsComplete` and calls `BindTexture` after the upload completes, slot-0 fallback in the gap.

That's a real architecture lift (UploadContext API extension + a deferred-bind pump with ownership rules between ctor/dtor/pump) — closer to L than the M the plan budgeted. Decision (recorded with user mid-execution): ship buffer-side migration this epic, defer the texture half. Buffer migration is clean — `UploadContext::UploadBuffer` has no layout-transition dependency, and queue submission order on the graphics queue ensures any draw consuming the buffer (always submitted later) implicitly observes the upload.

---

## Deferred follow-ups

### `texture-async-uploads` (next epic, not yet filed)

**Goal:** finish the texture half of S4. Runtime texture loads no longer block the calling thread.

**Scope:**
- Extend `UploadContext` with a non-blocking mip-chain submit. Currently `UploadImage` handles single-mip only; the new entry point records pre-barrier (UNDEFINED→TRANSFER_DST across all mips) → mip-0 staging copy → `vkCmdBlitImage` chain with per-mip transitions → final SHADER_READ_ONLY barrier → submit. Same `TimelineSemaphore` fence as today; same graphics queue today (UploadContext.cpp:51 currently uses `GetGraphicsQueue()`, which is the only family that supports `vkCmdBlitImage` with `VK_FILTER_LINEAR` per `VK_FORMAT_FEATURE_BLIT_DST_BIT` — so this stays graphics-queue forever, regardless of async-compute-queue v2.9.2's split).
- Add a deferred-bindless-registration pump composing with `AssetManager::s_UploadQueue` main-thread tick. Each `VKTexture` ctor that runs an upload pushes a pending entry `{u32* outIndex, VkImageView, VkSampler, u64 fenceValue}`; the pump iterates each frame, checks `UploadContext::IsComplete(fence)`, and calls `BindlessDescriptorSet::BindTexture` once ready. Until then `m_BindlessIndex` stays at `INVALID_BINDLESS_SLOT` and `Material::BindlessOrNull` keeps it sampling slot 0 (white fallback) — brief visual gap is the BACKLOG's intent.
- `VKTexture` dtor must remove its pending entry by view/sampler-pointer match; the pump must tolerate entries removed mid-iteration.
- Strip the inline `ImmediateSubmit` lambda from `VKTexture::CreateImage` (lines 217-327 in the data path). Keep `ImmediateSubmit` for the 5 init/control-flow sites that need synchronous semantics: `BindlessDescriptorSet::CreateNullTexture`, all `IBLPrecompute` GPU work, `PickingSystem` readback, `FrameDebuggerContext` capture, and any RT layout-transition path (line 347 in `VKTexture::CreateImage`).

**Phase-1 inventory required** (mandatory per CLAUDE.md plan-mode discipline):
- `AssetManager::s_UploadQueue` ownership rules — who pushes, who pops, what the per-tick contract is, whether the queue holds `shared_ptr<Asset>` for lifetime safety.
- All `VKTexture` ctor entry points (asset path, raw-data path, RT path, cubemap path) and which need deferred-bind vs. which already skip bindless registration (depth/cubemap/`R32_Uint`).
- Any code path that synchronously *expects* `m_BindlessIndex` to be valid immediately after ctor (UI texture preview, immediate model render). These need an explicit fast path or a documented brief-white-texture window.
- V3 affinity — if any cross-frame work involves the pump, confirm record/submit stays on the main thread.

**Effort:** L (vs. M budgeted for the original full S4).

**Deferred to a future plan-mode session** — issue body should be drafted then with the inventory above as input. Don't shortcut to a quick `gh issue create` from this epic.

---

## Build verification

Solution unchanged; rebuilt Debug x64 after each sub-task. No new warnings (the C4244 `_Rep` chrono cast warning in `Editor.cpp:402` was already present pre-epic).

**Smoke tests (recommended next):**
- **S1:** Release build with `-DLUTH_ENABLE_VALIDATION=1`, throwaway `appInfo.apiVersion = VK_MAKE_VERSION(99,0,0)` — confirm `LH_CORE_ERROR("Validation Layer: …")` fires before `LH_CORE_CRITICAL("Failed to create Vulkan instance!")`. Revert.
- **S2:** Load a scene with depth + cubemap + `R32_Uint` (entity-ID) textures alongside regular bindless textures; assert no `m_BindlessIndex == 0` collisions in `~VKTexture`; RenderDoc capture should show slot 0 still bound to the null white texture; dump a frame's `GPUMaterialData` SSBO via RenderDoc buffer view and confirm no field equals `0xFFFFFFFF`.
- **S3:** Resize the viewport repeatedly under different scene-color formats; pool size stays bounded (was unbounded under linear scan when usage flags differed). Hold viewport size for 31+ frames idle → cache shrinks.
- **S4:** Load a model-heavy scene; vertex/index uploads visible in Tracy as `UploadContext` zones, not `ImmediateSubmit`. No Vulkan validation errors (queue ordering on graphics queue is implicit).
- **S5:** Edit grid color in `RenderPanel` → live update next frame. Save settings, restart, confirm persistence. No `luthien/` includes leaked into `luth/source/luth/renderer/passes/`.
- **S6:** IBL probe still produces correct irradiance + prefiltered output; binary-diff `RenderDoc` capture of IBL pass at startup vs. baseline.
