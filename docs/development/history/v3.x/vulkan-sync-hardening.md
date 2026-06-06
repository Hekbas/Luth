# vulkan-sync-hardening (v3.0.11)

**Issue:** `fix: vulkan-sync-hardening` · **Branch:** `fix/vulkan-sync-hardening` · **Mode:** A (rt-renderer series, tag-only)

Drive the engine to a clean Vulkan synchronization-validation baseline before Phase C (ReSTIR DI/GI +
SVGF — the most cross-queue/temporal/barrier-heavy work on the roadmap), and build the observability that
makes the next sync break fast to diagnose. The `gpu-debug-toolkit` (#143, v3.0.10) surfaced latent
sync-val hazards the static-RT scene tolerated; this effort fixes them and lands the tooling that found
them.

## Approach — observability first, then sweep, then fix

The handoff proposed a manual `uncapped` sweep as step 1. That was reversed: a hand-driven sweep is
non-deterministic and lossy — the exact "half-blind" problem. Instead the legibility + introspection
tooling landed first, so the sweep produced a named, root-caused worklist instead of a `VkImage 0x7f3a…`
firehose.

| Sub-task | Commit | What |
|---|---|---|
| Object names + pass labels | `798e5b7` | Generalized `VulkanContext::SetDebugName` to any `VkObjectType` (typed overloads); `DebugUtilsFunctions` PFN cache mirroring `CheckpointFunctions`; `vkCmdBeginDebugUtilsLabelEXT` RAII-wrapping every RG pass body; named RG transients + BLAS/TLAS at creation. Makes RenderDoc/Nsight/Aftermath + validation legible. |
| Graph dump + barrier trace | `e099ec9` | `RenderGraph::DumpGraphDot()`/`DumpGraphJson()` (device-free serialization of passes/edges/solved barriers); `LUTH_RG_DUMP=<path>` one-shot; `LUTH_RG_TRACE` per-topology barrier trace; `BarrierReason` (RAW/WAW/FINAL) + `FindLastWriter` edge lookup. |
| Headless solver tests | `5f13aa2` | `tests/renderer/RenderGraphSolver.cpp` — doctest cases driving `RenderGraph::Compile()` with no live device (RAW / WAW-same-state / cross-queue / external-finalState). |
| Fix A — loadOp LOAD | `3410fa7` | Attachment `GetStateInfo` carries READ access alongside WRITE so a `loadOp LOAD` read is ordered after the prior attachment write. Exposed `GetStateInfo` + access-mask regression test. |
| Fix B — swapchain acquire | `faf21ce` | Moved the `imageAvailable` wait off view0-gA onto the swapchain-writing last-gB at `ALL_COMMANDS` (the `UNDEFINED→COLOR` transition is at `TOP_OF_PIPE`, which `COLOR_OUTPUT` wouldn't gate). |
| Fix A (cross-queue) | `f7fda7f` | Broadened compute←gA + gB←compute semaphore waits to `ALL_COMMANDS`; first-view gA waits the previous frame's compute (cross-frame shadow-map read). |
| Arch doc + version + this file | (wrap) | Corrected `arch/multi-queue.md`; FUTURE.md deferrals; `Version.h` → 3.0.11. |

## The sweep — real worklist

Built Debug, drove the editor (`character_test` scene: RT shadows + CSM + GTAO + volumetric + skinned +
multi-view + startup) under `LUTH_VALIDATION=sync,bp`, grouped `Luth.log` by message. Three
sync-correctness clusters surfaced — all three predicted by the toolkit notes:

- **A. loadOp LOAD read-after-write.** `GeometryPass` (depth, EQUAL test) + `GridPass` (color, in-place)
  do `Write(handle, LOAD_OP_LOAD)`, modeled by the solver as a *write* only. The attachment barrier's dst
  access was WRITE, so the `vkCmdBeginRendering` load *read* of prior contents was unsynchronized.
- **B. swapchain WRITE_AFTER_READ.** `ImGuiPass`'s `UNDEFINED→COLOR` transition raced
  `vkAcquireNextImageKHR`: the acquire semaphore was waited on a submit (view0 gA) that never touches the
  swapchain, and at `COLOR_OUTPUT`, which doesn't gate a `TOP_OF_PIPE` transition.
- **C. cross-queue WRITE_AFTER_WRITE.** Physical images written by both a graphics and a compute pass with
  no gating barrier (shadow map ↔ `VolumetricInjectScatter`; `SlimNormal`/`SceneDepth` ↔
  `RtSunShadows`/`GeometryPass`; volumetric froxel ↔ `VolumetricResolve`).

Plus best-practices noise (9× `UNDEFINED→read-only` discards, 6× small-buffer dedicated-alloc hints).

## Root cause — the cross-queue substitution was the bug

`arch/multi-queue.md` documented a "TOP_OF_PIPE substitution": a cross-queue barrier's src is forced to
`TOP_OF_PIPE`/`NONE`, trusting the submit-time semaphore. A multi-agent deep-research pass (Vulkan spec,
Khronos sync examples, LunarG SIGGRAPH 2021, Themaister, NVIDIA/AMD, UE5 RDG; 22 sources, 25 claims
adversarially verified) established the flaw:

- An empty (`TOP_OF_PIPE`/`NONE`) source scope forms an **execution-dependency chain only when the
  carrying semaphore's wait dst stage covers it**. A narrow wait (`COMPUTE_SHADER`/`FRAGMENT_SHADER`)
  leaves the reader's layout-transition *write* ungated — the C hazards, and the same mechanism as B.
- Broadening the cross-queue **semaphore wait** to `ALL_COMMANDS` (logically later than any stage) makes
  the chain form. **Genuinely spec-correct, not validation-silencing** — Khronos' own cross-queue
  layout-transition example uses `srcStage=NONE` only because it assumes `pWaitDstStageMask=TOP_OF_PIPE`.
- **QFOT is EXCLUSIVE-only.** CONCURRENT images (our policy) need no ownership transfer — Fix B (QFOT
  pairs) was ruled out as unnecessary ceremony.
- Production graphs (UE5 RDG) track *consumers* and place sync at the first consumer; our `lastWriter`-only
  solver can't. The full version needs per-pass semaphores — deferred.

## Fixes

- **A** — `RenderGraph::GetStateInfo` attachment states include the matching READ access bit, so every
  attachment barrier self-covers its `loadOp LOAD`. Reuses the existing read path; no new state.
- **B** — `imageAvailable` waited on the last gB (the only swapchain writer) at `ALL_COMMANDS`; first-view
  gA no longer waits it.
- **C (Fix A from the research)** — compute←gA + gB←compute waits widened to `ALL_COMMANDS` (gates the
  reader's cross-queue layout transition); first-view gA waits the previous frame's compute value
  (closes the cross-frame WAR on the single persistent shadow map). Per-pass barrier scopes left tight —
  only the submit-time waits widened, per the research's async-tax caveat.

## Verification

Each fix verified by re-sweep on the auto-restored scene (the editor's last session restores, so
self-launch exercises every feature path without manual driving). Final state:

- **Sync-val: 0** — A, B, C, and any RAW/WAR/WAW hazard all cleared, with the scene fully rendering (RT +
  GTAO + volumetric active, so C's zero is real).
- **GPU-AV: 0** — `LUTH_VALIDATION=gpuav` engaged (not no-op'd at the descriptor cap) and found no
  descriptor/OOB/uninitialized-access errors. The notes' "editor-startup uninitialized descriptor" does
  not reproduce.
- **Remaining: 16 best-practices, all confirmed-benign non-bugs** — the `UNDEFINED→read-only` warnings are
  `VKTexture`'s one-time legacy (`sync1`) creation transitions (written before sampled, GPU-AV-clean); the
  small-buffer hints are tagged-heap by design. Both tangential to sync-correctness; cleanup deferred
  (FUTURE.md).
- Solver tests: 5 cases / 17 assertions green (headless, no GPU).

## Decisions / deviations

- **lastReader/consumer tracking deferred.** It was approved up front as a forward-investment, but Fix A
  cleared the entire baseline on its own, removing the correctness justification. Building foundational
  solver machinery speculatively (the gap has no current repro or test case) was judged worse than
  deferring it to the ReSTIR/SVGF phase, where concrete cross-queue-WAR cases will exist to design + test
  against. Captured in FUTURE.md.
- **Best-practices not driven to zero.** The 16 remaining are confirmed non-bugs in tangential subsystems
  (VKTexture legacy-barrier creation path; VMA small allocs). Driving them to zero means a sync2 migration
  of VKTexture + VMA sub-allocation — out of scope for sync-hardening; deferred.

## Files

- `renderer/backend/vulkan/VulkanContext.{h,cpp}` — generalized `SetDebugName` + `DebugUtilsFunctions`.
- `renderer/backend/vulkan/VulkanBackend.cpp` — Fix B (swapchain acquire) + Fix A (cross-queue waits).
- `renderer/rendergraph/RenderGraph.{cpp,h}` + `RenderGraphResources.h` — pass labels, DumpGraph, trace,
  `BarrierReason`, `GetStateInfo` READ access (Fix A loadOp), `FindLastWriter`.
- `renderer/rendergraph/RenderResourceCache.cpp`, `backend/vulkan/{VulkanAccelerationStructure,TlasBuilder}.cpp`,
  `subsystems/RtSubsystem.cpp` — creation-site naming.
- `tests/renderer/RenderGraphSolver.cpp` — solver regression tests.
- `arch/multi-queue.md`, `FUTURE.md`, `Version.h` — docs + version.
