# gpu-device-lost

**Date:** 2026-06-06
**Series:** `rt-renderer` (Mode A). Folded into **v3.0.11** (`vulkan-sync-hardening`) — the device-lost hunt completed that held effort, so the whole branch ships under one `v3.0.11` tag. No bump, no Release.
**Branch:** `fix/vulkan-sync-hardening` (the held v3.0.11 sync-hardening + GPU-debug tooling rides in on this merge).

---

## Overview

The skinned-character `VK_ERROR_DEVICE_LOST` (TDR) that kept recurring in Release — hunted and thought-fixed
twice before — is root-caused, fixed structurally, and two adjacent bugs it had been *masking* are fixed
along with it. The hunt ran on the held `fix/vulkan-sync-hardening` branch because that branch's v3.0.11
GPU-debug kit (object names, `LUTH_RG_DUMP`, Aftermath re-arm, `LUTH_VALIDATION` tiers) was exactly the
diagnostic toolkit needed — so this effort also lands the sync-hardening merge.

**Diagnosis.** Re-armed Nsight Aftermath (it's `AFTERMATH_SDK`-env-gated in premake; a prior regen had
silently compiled it out — the "no dump" that stalled the previous hunt) and reproduced. The fresh dump
named it unambiguously: an MMU **read** page-fault in the skinned **vertex** shader at a **4.00 MiB buffer**,
with a page-fault-resource history of *five* 4 MiB buffers churning at one VA per frame — the
`BoneMatrixBuffer` dual-region. At 4 MiB it exceeds the 2 MiB page size, so it takes the tagged-heap
**large-one-shot path, which `vkDestroyBuffer`s its dedicated buffer immediately on `FreeTag`** — so a
marginally-late read on the true-async-compute RTX 3080 (`compute=2 (async)`) hit an unmapped VA. The page
path is safe (recycles backing, never destroys); only the large path destroyed.

**The fix shape — from Naughty Dog.** A deep-research pass (Gyrling GDC 2015 deck decoded verbatim,
cross-checked vs NVIDIA CTransientBuffer / id-DOOM / Granite / VMA; 25/25 claims verified) established the
principle: ND's tagged heap has **no per-allocation free** — only bulk-recycle-by-tag gated on frame
completion — and large allocations are **not** special-cased into destroyable objects (they get consecutive
blocks from the same never-destroyed heap). A too-early reclaim must degrade to *stale data*, never an
unmapped-VA fault. So the fix is to **never destroy per-frame GPU buffers** — recycle them, exactly as the
page path already does. See [[reference_nd_gpu_memory_model]].

---

## Sub-tasks

| # | What landed | Commit |
|---|---|---|
| A | **Recycle large-one-shot buffers (crash fix + safety net).** `GPUTaggedPageAllocator::FreeTag` pools `isLargeOneShot` pages into `m_FreeLargePages` instead of `FreeBuffer`+`LH_DELETE`; `AllocateLargeTagged` reuses an exact-size pooled buffer before allocating fresh; `Shutdown` drains the pool. A too-early reclaim now yields stale data, not a TDR. Covers bones (4 MiB) + the forward-plus light-index (3.375 MiB). | `4da0193` |
| B | **Direct completion-gated reclaim (ND `HasFrameCompleted`).** Replaced the opaque `FreeTag(frameIndex-4)` formula with a direct `IsFrameComplete`-gated sweep: free tag `label-1` for each GPU-complete consuming frame, high-water-marked by `m_LastReclaimedLabel`. Dropped the bone `gameFrame+1` tag offset (the very offset that admitted the original UAF) — tag now = consuming frame, uniform with every producer. | `e3e0d30` |
| C | **Remove vestigial machinery.** Deleted `VulkanWaitJob.{h,cpp}` (never dispatched) + `FrameContext::GpuFinished`/`GpuTimelineValue` (never set/read); `IsFrameComplete` promoted to the normative reclaim predicate. | `481813e` |
| D | **Arch docs.** `arch/memory.md` lifetime table + the large-one-shot recycle correction + the unified tag rule; `arch/multi-queue.md` reclaim predicate. | `528b56d` |
| E | **AS-build input barriers.** *(caught by the validation sweep)* Geometry-input reads of the deformed-VB sync with `SHADER_READ` per spec, not `ACCELERATION_STRUCTURE_READ` — clears a sync-val `READ_AFTER_WRITE` on the skinned-BLAS initial build + per-frame refit. | `1e2fa19` |
| F | **Per-frame descriptor-reuse wait.** *(the skinned-pose ghost)* The block-wait covers the descriptor slot's prior *reader* (frame N-3), not just its cmd-buffer prior user (N-4) — closing a UAB slot-reuse race the device-lost crash had been masking. | `a8364fe` |

---

## The two bugs the crash was masking

Killing the TDR let the skinned scene render in steady state for the first time (it had been crashing at
~57 s), surfacing two pre-existing issues:

### AS-build input barriers (sync-val)
The validation sweep on the re-armed build (`LUTH_VALIDATION=sync`/`bp`/`rt` — clean except this) flagged a
`READ_AFTER_WRITE` on `vkCmdBuildAccelerationStructuresKHR` reading the deformed vertex buffer. The barrier
existed but used `ACCELERATION_STRUCTURE_READ` for a *geometry-input* read; the spec requires `SHADER_READ`
for vertex/index build inputs (`ACCELERATION_STRUCTURE_READ` is for AS objects + scratch). Two access-mask
bits — initial build + per-frame refit. *(`gpuav` crashes on this bindless+RT engine — a documented
descriptor-cap tooling limitation, not an engine defect; sync/bp/rt cover the surface. The benign
`nvoglv64.dll` path-case and "sub-allocate small buffers" messages are layer noise.)*

### The skinned-pose ghost (a Heisenbug)
A Release-only, animation-dependent black "ghost respecting depth" on the skinned character — amplified by
multi-view + shadows, **gone under RenderDoc** — the textbook signature of a CPU-ahead/GPU-behind timing
race. Ruled out (with the user's testing) motion vectors (camera motion on static geometry was clean) and
TAA/GTAO/fog (disabling them didn't help). Root cause: per-frame UAB descriptor slots are read at
`renderFrameIndex % N`, but the block-wait only guaranteed the *cmd-buffer* slot (`gameFrameIndex % N`) — one
frame older — was GPU-done. Under heavy load (GPU 3 frames behind) a game-stage `vkUpdateDescriptorSets`
landed while an older in-flight render frame still read that slot → mismatched bone poses across passes →
depth-rejected fragments → black, depth-respecting. Visible on bones because that data changes every frame;
the same race class as the original device-lost and the v2.8.9 material flicker. Fix: wait the slot's prior
*reader* (N-3). Monotone, so it still covers cmd-buffer reset; systemic (fixes every cycled descriptor);
trades one frame of CPU-ahead pipelining (3→2, the Vulkan-tutorial default — and slightly *better* input
latency) for correctness.

---

## Architectural decisions

### Recycle, never destroy — the ND structural property
The crux of the research: in ND's model a too-early reclaim recycles arena memory (stale data at worst) and
**can never produce an unmapped-VA fault**, because there is no destroyable per-allocation object. The
engine's page path already had this property; the large-one-shot path violated it the moment the bone buffer
crossed 2 MiB (the `eba42ad` dual-buffer prev-bones change turned a harmless page-recycle into a hard
`vkDestroyBuffer`). a2 restores parity. Considered and rejected: contiguous multi-page allocation (ND's
literal mechanism) — the page free-list is flat/unordered with no contiguity tracking, so it would need
real hot-path surgery; pooled-recycle achieves the identical structural property surgically.

### Direct `IsFrameComplete` reclaim over the `-4` formula
The legacy `FreeTag(frameIndex-4)` plus the ad-hoc bone `+1` tag offset *hid* a render/game off-by-one behind
opaque arithmetic — and that offset is precisely what admitted the original UAF. The direct sweep frees tag
`T` exactly when `IsFrameComplete(T+1)` (the submit that consumed render-frame-`T` data) is complete, on both
graphics and compute timelines. The tag now equals the consuming frame for every producer; the block-wait +
V6 overflow tier are retained verbatim. Subtractive: removed the formula and `VulkanWaitJob`, added no
primitive — `m_FreeLargePages` is the large analogue of `m_FreePages`.

### Descriptor reuse gated on the prior reader
The descriptor read-slot (`renderFrameIndex % N`) is offset exactly one frame from the cmd-buffer slot
(`gameFrameIndex % N`) the block-wait protected — invariant under `MAX_FRAMES_IN_FLIGHT`, so bumping the
in-flight count does *not* fix it. UAB silenced the validation error (VUID 03047) but never the data hazard;
the bone-descriptor comment even named "the GPU falls behind" as the reason it used UAB. The fix shifts the
block-wait's retiring slot by one frame to cover the descriptor's prior reader. The alternative (one extra
descriptor slot, `MAX+1`, no latency cost) is recorded as the zero-perf-cost option if profiling ever shows
the shallower pipeline matters.

---

## Files touched

**Engine:**
- [`GPUTaggedPageAllocator.{h,cpp}`](../../../luth/source/luth/memory/GPUTaggedPageAllocator.cpp) — `m_FreeLargePages` recycle pool (A)
- [`VulkanBackend.{h,cpp}`](../../../luth/source/luth/renderer/backend/vulkan/VulkanBackend.cpp) — direct `IsFrameComplete` reclaim sweep + `m_LastReclaimedLabel` (B); descriptor-reuse block-wait (F)
- [`BoneMatrixBuffer.cpp`](../../../luth/source/luth/renderer/resources/BoneMatrixBuffer.cpp) — drop the `gameFrame+1` tag offset (B)
- `VulkanWaitJob.{h,cpp}` + [`FrameData.h`](../../../luth/source/luth/core/FrameData.h) — dead-code removal (C)
- [`VulkanAccelerationStructure.cpp`](../../../luth/source/luth/renderer/backend/vulkan/VulkanAccelerationStructure.cpp) + [`RtSubsystem.cpp`](../../../luth/source/luth/renderer/subsystems/RtSubsystem.cpp) — AS-build input barriers → `SHADER_READ` (E)

**Docs:** `arch/memory.md`, `arch/multi-queue.md`.

---

## Verification

- **Release Aftermath soak** — Walking scene, **10 min clean** (was failing ~57 s): zero device-lost. Bones 4 MiB + light-index 3.375 MiB recycle under load. *(user)*
- **Validation sweep** — `LUTH_VALIDATION=sync` / `bp` / `rt` clean on the skinned scene after the AS-barrier fix (re-validated; the `READ_AFTER_WRITE` is gone).
- **Skinned-pose ghost** — gone after the descriptor-reuse wait, confirmed in the worst case (both panels + shadows + animation). *(user)*
- **Build** — Debug + Release x64 clean, 0 errors; only pre-existing warnings.

## Notes for the maintainer

- Aftermath is env-gated: `set AFTERMATH_SDK=<path>` before `premake`, or it silently compiles out. Path on this box recorded in [[reference_aftermath_setup]].
- The descriptor-reuse race is the same class as the original device-lost + the v2.8.9 material flicker — every per-frame `renderFrameIndex % N` cycled descriptor now relies on the prior-reader block-wait. If a future change touches `AcquireImage`'s retiring-slot math, re-check it against the skinned ghost under multi-view + shadow load.
