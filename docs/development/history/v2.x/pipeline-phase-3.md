# v2.8.4 — pipeline-phase-3

**Date:** 2026-04-26
**Commits:** 18 (on `refactor/pipeline-phase-3`)
**Issue:** [#96](https://github.com/Hekbas/Luth/issues/96)

---

## Overview

Made the documented `Game(N) | Render(N-1) | GPU(N-2)` CPU pipeline actually
execute concurrently. Pre-v2.8.4 the structure existed (triple-buffered
`FrameContext`, separate stage entry points) but stages ran sequentially
within the frame — the App.cpp banner explicitly said so. v2.8.4 closes that
gap: stages dispatch onto worker fibers, the frame boundary becomes a
`RenderSnapshot` POD captured at end of game stage, and the render stage of
frame N-1 fires before the wait on Game(N)'s counter.

Also delivered along the way: a JobSystem hardening pass (four bugs surfaced
under sustained concurrent-stage load), a two-phase render-graph dispatch
that defused the resulting compounding yield pressure, stage-isolation asserts
on the subsystems that intentionally retained mutexes, and a stack of
incidental editor / FrameDebugger / panel-resize fixes that the new
concurrency exposed.

Within-stage parallel pass recording (Tier 2) is deferred — the two-phase RG
yields once per frame regardless of pass count, but pass execute lambdas
themselves still record their secondary cmd buffers serially. That tier
needs the next fiber-system polish pass.

---

## Architecture decisions (locked at spec time)

| # | Decision |
|---|---|
| D1 | Frame N renders against frame N-1's snapshot (`FrameContext::Previous()`). |
| D2 | Hand-built `RenderSnapshot` POD on `FrameContext`, backed by `LogicMemory` (`std::span` + `LinearAllocator`). |
| D3 | `RenderingSystem::Update` wraps as one render-stage job. RG keeps per-pass dispatch but collapses to one wait. |
| D4 | Editor on main thread for ImGui draw; ECS mutations precede game stage. |
| D5 | Frame 0 nominally skips render. Shipped impl runs frames 0/1 sync against `Current()` instead — see Deviations. |
| D6 | `MaterialSystem` + `BoneMatrixBuffer` mutexes retained, NOT converted. Stage-isolation discipline + assert. |

---

## Sub-tasks shipped

| # | Sub-task | Commit |
|---|---|---|
| S1 | RenderSnapshot scaffold | `29aa6fa` |
| S2 | CaptureSnapshot impl | `5031177` |
| S3 | DrawListBuilder + GPUObjectBuffers → snapshot | `8261b6b` |
| S4 | LightGatherer → snapshot | `2f65ad3` |
| S5 | Frame Debugger tag lookups → snapshot | `7e755c7` |
| S6 | MaterialSystem::Update → game stage | `437cefa` |
| — | Drop CaptureSnapshot trace log | `5dcdefb` |
| S7 | Wire GameReady/RenderReady counters (sync waits) | `41e8bc3` |
| S7-fix | WorkStealingDeque TryPop bottom restore on race | `077ae44` |
| S7-fix | Preserve FP state across fiber switches | `c1a2251` |
| S7-fix | SystemRegistry typed-hash slot lookup | `a5d24eb` |
| S7-fix | Pin fibers + scaffold Tracy fiber instrumentation | `b9a53e4` |
| S8 | Concurrent Game(N) \| Render(N-1) dispatch | `ed090a9` |
| S8-hang | Two-phase RG dispatch (1 yield/frame) | `2a8efc7` |
| S9 | Stage-isolation asserts | `ebe9cab` |
| inc | Release ViewResources + drain GPU at panel resize | `edf5348` |
| inc | Serialize RG per-pass when FrameDebugger captures | `52763b0` |
| S10 | Per-stage timings + ProfilerPanel bars | `80724b2` |
| S11 | Retire `entt::registry&` from render path | `01934f2` |
| chore | Trim epic-marker noise from comments | `7be0525` |

---

## Key design notes

### Why a snapshot, not registry sharing

Pre-v2.8.4 the render path called `registry.view<...>` directly. With S8's
concurrent dispatch, game-stage component mutations (transform updates,
animation pose writes) would race against render-stage reads. Solving it via
locks would re-introduce the very contention this epic exists to remove.

The `RenderSnapshot` is a POD frozen at end of game stage, after all ECS
mutations for the frame are done. Spans live in `FrameContext::LogicMemory`
(reset two iterations later, by which time the GPU has finished consuming
that frame). The render stage walks the snapshot, never the registry —
audited at S11 close: `grep -rn "entt::registry\|registry\." luth/source/luth/renderer/`
returns zero matches.

### `RenderFrame()` accessor

`FrameContext::Current()` is "the slot game writes to this iteration".
`Previous()` is "the slot game wrote last iteration". With S8 concurrent
dispatch, the render stage targets `Previous()` in steady state but
`Current()` during the frame-0/1 sync warm-up. `FrameData::RenderFrame()`
returns whichever the render stage should read this iteration, set by App
before each `RenderStageFn` dispatch.

### Stage tags + asserts (D6)

`MaterialSystem` and `BoneMatrixBuffer` keep their mutexes — converting
them to lock-free structures was out of scope. The stage-isolation
discipline is enforced by `JobSystem::Stage` propagating from the
dispatching parent fiber to children via `Job::StageTag`, applied at
`FiberEntryPoint`. `MaterialSystem::Register/Update/Unregister` and
`BoneMatrixBuffer::Allocate/Free/UploadBones` `assert(GetCurrentStage() == Stage::Game)`.
Inline-execution paths save/restore `CurrentStage` so an inline-run job
runs under its own tag and doesn't pollute the calling fiber.

### Two-phase render-graph dispatch

The S8 smoke gate hung at ~2 min into a sustained animated-character session,
mid `ShadowPass.C3` recording. Diagnosis: `RG::Execute` dispatched one
`RGPassRecord` job per pass and waited per-pass — ~22 yields per render frame
in steady state. Each yield interacts with fiber pinning (added in `b9a53e4`
for Tracy zone correctness) and `WakeByAddressSingle`'s any-thread wake
semantics; under sustained load the wake-targeting friction compounded into a
hang. Fix: phase 1 dispatches every `RGPassRecord` against a single counter,
single `WaitForCounter`; phase 2 emits the primary cmd buffer in pass order.
1 yield per render frame. 14-min smoke clean post-fix.

The same fix surfaced a thread-safety bug in `FrameDebugger`:
`BeginCapturePass`/`CaptureDrawCall` push into shared
`capturedFrame.passes`/`drawCalls` vectors. Pre-`2a8efc7` dispatch was serial
so the race never fired. Post-`2a8efc7` it's a deterministic crash on the
first capture. Resolution: `RenderGraph::SetSerialize(bool)` flag.
`RenderPipeline` sets it on every view's RG when FrameDebugger is in
`CaptureRequested` state; serial mode reverts to the per-pass dispatch+wait
shape for the duration of the capture.

---

## Bugs surfaced + fixed along the way

The S7 smoke gate was the first time game/render stages had ever run on
worker fibers (pre-v2.8.4 they ran inline on main). That exposed four
JobSystem bugs that had been latent:

- **`077ae44`** — `WorkStealingDeque::TryPop` race-path. `compare_exchange_strong`
  overwrites its expected-by-reference parameter on failure, so the lost-race
  path was storing `Bottom = origTop + 2` instead of the Chase-Lev invariant
  `Bottom = origTop + 1`. Phantom slot whose later reuse made future pops
  re-execute a Job with a now-dangling data pointer.
- **`c1a2251`** — `FIBER_FLAG_FLOAT_SWITCH`. Without it, x87/MMX/XMM state
  isn't preserved across fiber switches. Only surfaced when nested system
  Updates yielded often.
- **`a5d24eb`** — `SystemRegistry`. Replaced `dynamic_cast` scan with
  typeid-hash slots (O(1), no RTTI walk, no debug-iterator contention).
- **`b9a53e4`** — Fiber pinning. Yielded fibers now resume only on their
  originating worker so per-thread Tracy zone state stays consistent. Tracy
  fiber Enter/Leave instrumentation in place, gated on `TRACY_FIBERS` (still
  undefined — re-enabling crashes Tracy; needs another instrumentation pass).

The S8 smoke gate exposed three more, all incidentals that the new
concurrency made deterministic:

- **`edf5348`** — Project-switch + scene-load descriptor cascade. Reproducer:
  open project A, switch to B, switch back to A, load any scene. Game panel
  rendered with descriptor sets pointing at already-destroyed `VkImageView`s.
  Two-fold fix: `FrameTargets::Resize` early-outs on no-size-change so it
  doesn't blindly replace texture shared_ptrs, and both `RenderingSystem::Resize`
  and `GamePanel`'s resize callback `WaitForGPU` then `ReleaseViewResources`
  before allocating new scene textures.
- **`52763b0`** — FrameDebugger crash on capture (described above).
- **wake-targeting attempt + revert** — Tried
  `WakeByAddressAll + fetch_add` in `DecrementCounter` to address
  hypothetical wake misses; regressed launcher responsiveness immediately
  (saturation under high wake-rate). Reverted in favor of the two-phase RG
  approach which addresses the root cause (yield count) rather than wake
  targeting (a symptom). Documented in spec follow-ups.

---

## Files touched (selected)

**Core / frame:**
- `luth/source/luth/core/RenderSnapshot.{h,cpp}` (new)
- `luth/source/luth/core/FrameData.h` — `RenderSnapshot` field on
  `FrameContext`, `RenderFrame()`/`SetRenderFrameIndex()` accessors,
  defensive `AtomicCounter::Lock` clear in `Reset()`
- `luth/source/luth/core/App.{h,cpp}` — Run() restructured around
  `Execute(GameStageFn)` + `Execute(RenderStageFn)` with frame-0/1 sync
  warmup and frame ≥2 steady. `Renderer::WaitForGPU()` at LoadProject

**JobSystem (`luth/source/luth/jobs/`):**
- `JobSystem.{h,cpp}` — `Stage` enum, `JobContext::CurrentStage`,
  `Job::StageTag`, parent-stage capture in `Execute`/`Dispatch`,
  inline-execute save/restore, `RecordStageTime` API + `Stats.GameStageMs/RenderStageMs`
- `Fiber.h` — `FIBER_FLAG_FLOAT_SWITCH`, `PinnedThreadIndex` (+ pin enforcement
  in `WorkerThreadLoop` ready-pickup)
- `WorkStealingDeque.h` — Chase-Lev `TryPop` bottom restore fix

**Renderer (`luth/source/luth/renderer/`):**
- `rendergraph/RenderGraph.{h,cpp}` — two-phase Execute; `SetSerialize` flag
- `RenderPipeline.{h,cpp}` — `Execute(view, primaryCmd)` (no registry);
  `SetSerialize(true)` per-view when FrameDebugger captures
- `passes/{DepthPrepass,GeometryPass,SelectionPass,ShadowPass}.cpp` — drop
  `entt::registry&` from signatures and lambda captures
- `DrawListBuilder.{h,cpp}` + `gpu/GPUObjectBuffers.cpp` — read snapshot
- `lighting/LightGatherer.{h,cpp}` — read snapshot
- `material/MaterialSystem.cpp` — `assert(Stage::Game)` on mutators
- `resources/BoneMatrixBuffer.cpp` — `assert(Stage::Game)` on mutators
- `FrameTargets.cpp` — no-change early-out

**Scene (`luth/source/luth/scene/systems/`):**
- `RenderingSystem.{h,cpp}` — `RecordView(view, primaryCmd)` (no registry);
  `m_ActiveSnapshot` member; `Resize()` drains GPU + drops ViewResources
- `SystemRegistry.{h,cpp}` — typeid-hash slot lookup

**Editor (`luthien/source/luthien/`):**
- `panels/GamePanel.cpp` — resize callback drains GPU + drops ViewResources
- `panels/ProfilerPanel.{h,cpp}` — Game / Render bars between CPU and GPU

---

## Build verification

Debug x64 clean across all 18 commits (zero new errors; pre-existing MSVC
STL conversion warnings + `getenv` deprecation noise unchanged).

Long smoke (10+ min, animated character, project A↔B↔A round-trips, scene
load, material swap, FrameDebugger capture) clean.

Per-stage timings visible on ProfilerPanel between the CPU and GPU bars;
Tracy zones distinct for `GameStage` and `RenderStage` via the existing
`LH_PROFILE_SCOPE_DYNAMIC_CSTR(jobPtr->Name)` in `FiberEntryPoint`.

---

## Deviations from spec issue #96

- **D5 — frame 0 sync, not skipped.** The spec wrote frame 0 as "skip
  render entirely". Skipping the iter-0 submit desyncs Vulkan's
  `m_FrameTimeline` (iter-3's `Wait(value 1)` deadlocks since iter 0 never
  signaled) and leaves a never-waited-on entry in the imageAvailable
  semaphore ring. Fix would require a backend tweak
  (`TimelineSemaphore::Init(1)` + skip iter-0 `AcquireImage`) outside the
  S8 file scope; deferred. Net behavior from iter 2 onward is identical to
  the spec's intent — steady-state pipelining engages exactly where expected.
- **Within-stage parallel pass recording (Tier 2).** Deferred per the spec
  goal statement; the two-phase RG drops yield pressure but each
  `RGPassRecord` still records sequentially within the dispatched job.
  Future epic.

---

## Known limitations / follow-ups

- **`TRACY_FIBERS` instrumentation.** Skeleton in place (gated on the
  define). Re-enabling crashes Tracy; needs another instrumentation pass
  to understand the crash-with-Tracy-connected case.
- **Backend timeline init for true frame-0 skip.** Small backend tweak
  that would let S8 actually realize spec D5. Low priority; net visual
  behavior is identical.
- **Wake-targeting under fiber pinning.** The two-phase RG defused the
  yield-count compound, but the underlying interaction (pinning vs
  `WakeByAddressSingle` any-thread wake) is still latent. If a future
  workload reintroduces high yield rates per render frame, options on
  the shelf: per-worker wake addresses, or move bone matrices into the
  per-frame snapshot (away from the shared mapped GPU buffer — the ND
  `FrameParams` pattern).

---

## Next

`frame-debugger-polish` (#92, v2.8.5) — address the FrameDebugger
order-of-passes / output-image issues that surfaced (and aren't from this
epic). Then `animation-quick-pass` (#93, v2.8.6).

## Related docs

- `docs/development/arch/fiber-system.md` — V1-V6 hazard glossary
- `docs/development/arch/frame-pipeline.md` — frame model (rewritten in S12)
- `docs/development/arch/version-glossary.md` — V<n> markers
- Prior art: `engine-consolidation` (v2.8.2) for Tracy global hooks;
  `tracy-on-demand` (v2.8.3) for the Tracy queue fix
