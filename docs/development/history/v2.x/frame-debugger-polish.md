# v2.8.6 — frame-debugger-polish

**Date:** 2026-04-28
**Commits:** 14 (on `refactor/frame-debugger-polish`)
**Issue:** [#92](https://github.com/Hekbas/Luth/issues/92)

---

## Overview

Polish pass on the Frame Debugger after `play-mode` (v2.8.0) and `game-panel` (v2.8.1) shipped. Originally scoped S, grew to L over the course of the work — the four #92 checklist items (slider, partial-frame replay, Game/Editor switch, capture across play states) all landed, but a chain of bugs surfaced during verification each shipped its own fix: pass→archive index keying, graph-order pass sort, stable EventTree IDs, archive image reuse + throttle eliminating an editor freeze under continuous camera movement, and metadata suppression for non-capturing views fixing a JobSystem deadlock.

The big visible additions are the **Unity-style draw-level scrub slider** (the previous slider was pass-level only) and the **viewport pass overlay** (the editor or game viewport renders the selected pass's archived RT instead of the live frame, with the overlay target coupled to the capture source). Per-draw replay dispatch was refactored to support extension beyond GeometryPass; the actual Shadow/DepthPrepass/SelectionMask replay bodies are deferred to a follow-up effort ([#100](https://github.com/Hekbas/Luth/issues/100)) — the dispatch + key-check infrastructure ships, callers gracefully fall back to pass archive when replay isn't valid for a given pass.

Tag-only release; not a milestone.

---

## Sub-tasks

| # | Sub-task | Commit |
|---|---|---|
| A1 | Fix `passArchives` index keying (sparse vs dense divergence) | [`1dd5b25`](../../../../commit/1dd5b25) |
| A1b | Sort captured passes by graph registration order at `FinalizeCapture` | [`7b72475`](../../../../commit/7b72475) |
| B  | Pass-scrub slider in `FrameDebuggerPanel` (later replaced by I1) | [`6686338`](../../../../commit/6686338) |
| C  | Unity-style viewport pass preview (Scene/Game viewport overlay) | `6067a6f` → reframed in [`4d58de1`](../../../../commit/4d58de1) (capture-source coupling, dropdown UX, freeze investigation start) |
| G  | Reuse archive images across recaptures + 10 Hz auto-recapture throttle + non-capturing-view metadata suppression (fixed editor freeze under continuous camera movement) | [`c823f77`](../../../../commit/c823f77), [`27348ba`](../../../../commit/27348ba), [`4bb6100`](../../../../commit/4bb6100) |
| F  | Stable EventTree node IDs (kind+identity-hashed PushID instead of traversal counter) | [`3b7f33a`](../../../../commit/3b7f33a) |
| H  | Depth-archive viewport overlay (route depth through `BlitArchivedDepthToPreview`) | [`ce15d73`](../../../../commit/ce15d73) |
| I1 | Unity-style draw-scrub slider over `drawCalls` + tree-click snap to `lastDrawIndex` | [`5500ac1`](../../../../commit/5500ac1) |
| I2 | Per-draw replay dispatch refactor (Shadow/Depth/Selection bodies deferred to [#100](https://github.com/Hekbas/Luth/issues/100)) | [`21b499e`](../../../../commit/21b499e) |
| D  | Smoke-test capture across Editing/Playing/Paused/Paused+Step | (no code — all four states verified clean) |
| –  | Comment-block tightening per CLAUDE.md (verbose narratives moved to this file) | [`e29c650`](../../../../commit/e29c650) |
| E  | Wrap-up (this file, version bump, ROADMAP/BACKLOG/CLAUDE.md updates, merge + tag) | (this commit) |

A2 (drop `SetSerialize`, add metadata mutex) was deferred during planning; the `4bb6100` non-capturing-view metadata suppression in the G chain mitigated the underlying SetSerialize cost enough that A2's full scope wasn't justified for v2.8.6.

---

## Investigation arcs

### A1 — pass→archive index keying

`CapturedFrame::passArchives` was keyed by **graph pass index** (sparse — empty slots for culled passes / passes without tracked-RT writes), but `CapturedFrame::passes` was **dense** (push order, only passes that called `BeginCapturePass`). `BuildPassNode` and the panel's "Pass Outputs" listing both fed the dense index into the sparse vector — once the two diverged by even one slot, every later pass-archive lookup was shifted, producing the original symptom: panel selection and metadata coherent, but the displayed image off by one.

Fix added a `passIndex` field to `RenderPassContext`, threaded `(u32)i` through Phase 1 + Phase 2 lambda invocation, stored as `CapturedPass.graphPassIndex` via a new first parameter on `BeginCapturePass` (16 call sites). `BuildPassNode` and `FrameDebuggerPanel.cpp:854-859` route through `pass.graphPassIndex` for archive lookups.

### A1b — graph-order pass sort

After A1's keying fix, a *second* ordering symptom became visible: the Frozen tree listed all graphics passes first, then all compute passes, instead of graph order. Cause is the two-phase RG dispatch — Phase 1 records graphics secondaries (calling `BeginCapturePass` during recording) before Phase 2 emits the primary, and compute passes bypass Phase 1 to execute inline in Phase 2. So compute pushes arrive after every graphics pass.

Fix is a `std::stable_sort` of `capturedFrame.passes` by `graphPassIndex` at `FinalizeCapture`, with an inverse-permutation remap of `drawCall.passIndex` so the back-references stay valid post-sort.

### Freeze investigation (C → G arc)

Sub-task C shipped the viewport overlay; user reported a hard editor freeze (no crash, no Vulkan validation errors) after ~5 seconds of continuous editor camera movement with both Scene + Game panels open and a pass selected. Stack-pause showed the render fiber yielded inside `JobSystem::WaitForCounter` with the per-frame counter stuck at 2.

Root cause was per-frame resource churn under the auto-recapture-on-camera-move trigger:

- `BeginCapture` → `DestroyArchives` queueing ~10 `VkImage` + `VkImageView` + `VmaAllocation` for deferred deletion every frame
- `OnPassExecuted` → 10 fresh VMA allocations every frame
- ~3 fresh `vkAllocateDescriptorSets` (panel thumbnail + Scene viewport overlay + per-draw preview) every frame as panel/viewport caches kept missing on new view pointers

Plus the SetSerialize → ~30 nested fiber dispatches per recapture frame (game view AND scene view both serialized due to a coarse gate keyed only on `state == CaptureRequested`).

Fix landed in three commits:

1. **`c823f77` — archive image reuse.** `BeginCapture` no longer calls `DestroyArchives`. `OnPassExecuted` becomes find-or-allocate via a new `m_ArchiveSlotMap` keyed on `(passName, rtName)`. Same VkImage / VkImageView pointer across captures → panel and viewport descriptor caches stay valid frame-to-frame. `EmitArchiveCopy` already supported re-copy via `dst.currentLayout` (anticipated by the original `ArchivedImage.h:32-34` design comment). Reduced freeze time from 5s → 10s.

2. **`27348ba` — 10 Hz throttle.** Reuse alone halved the cost but each recapture still issued ~10 `vkCmdCopyImage` + ~40 barriers (mostly shadow cascade copies) — at 60 Hz that bandwidth saturated mid-tier GPUs. Track `lastRecaptureFrameIndex` on `FrameDebugger`; gate the cameraMoved trigger on a 6-frame minimum interval. Camera-move feedback at 10 Hz is visibly stepped at very rapid motion but stays smooth at typical navigation speeds.

3. **`4bb6100` — non-capturing-view metadata suppression.** Throttle alone wasn't sufficient — debugger pause-stack showed `WaitForCounter` stuck at value=2 with the render fiber yielded. SetSerialize was applied to *every* view's RG when state was CaptureRequested; non-capturing queued views (e.g. Game view when source = Scene) ran SetSerialize unnecessarily because their lambdas would otherwise race on shared `FrameDebugger` metadata vectors, but those pushes are wasted (cleared by the capturing view's `BeginCapture` later). At 10 Hz × ~30 nested fiber dispatches per recapture frame, the JobSystem hit an edge case and the render fiber's per-pass counter never decremented. Fix gates SetSerialize on `view.captureRequested` AND temporarily masks `FrameDebugger.state` to `Inactive` around non-capturing views' `RecordGraph` call so their lambdas skip pushes entirely. Half the nested fiber dispatches per recapture frame; counter no longer gets stuck.

### Capture-source coupling (C-amend)

The original C interpretation of #92's "Game vs Editor view switch during scrub" was a panel-side overlay-target combo (Off/Scene/Game/Both) with capture always pulling from the editor scene view. The user pointed out that this was incoherent — the Game viewport could end up overlaying the editor camera's pass output, useless for camera-specific debugging. Reframed: capture source and overlay target are **coupled** by design.

`4d58de1` adds a `RenderView::captureRequested` flag (set by the view's owner — `RenderingSystem` for sceneView, `GamePanel` for gameView), `Luth::CaptureSource` enum, and `FrameDebugger::{requestedSource, capturedSource}`. `requestedSource` is the user's pending pick (panel writes); `capturedSource` is stamped at finalize so toggling source mid-Frozen doesn't redirect the live overlay. Auto-recapture-on-camera-move is skipped when `capturedSource == Game` because `m_CameraParams` (editor camera) doesn't match the captured viewProj (game camera) — comparing them would loop the state machine every frame.

The same commit also moved the source picker from radio buttons to a single dropdown placed immediately right of the Enable/Disable button, in both control bars, so its position is stable across the live↔capture transition.

---

## Architectural changes

- **`RenderPassContext::passIndex`** — new u32 field set in both Phase 1 and Phase 2 of `RenderGraph::Execute`. Plumbed through `BeginCapturePass` so each `CapturedPass` records its `graphPassIndex`. Used by the sort in A1b and by lookups in `BuildPassNode` / panel.

- **`CapturedPass::graphPassIndex`** + `EventNode::lastDrawIndex` — new fields. The first identifies the source pass in the original RG; the second supports Unity's tree-click-snap behavior for the draw scrub slider. Populated post-order in `BuildEventTree`.

- **`FrameDebugger::m_ArchiveSlotMap`** — `unordered_map<string, u32>` keyed by `(passName + "/" + rtName)` → index in `archivedImages`. Survives across captures so `OnPassExecuted` reuses existing VkImages instead of allocating fresh ones every recapture. Cleared only by `DestroyArchives` (full cleanup on `ExitCapture`).

- **`RenderView::captureRequested`** + `Luth::CaptureSource` enum + `FrameDebugger::{requestedSource, capturedSource}` — couples the capture source to the overlay target. Panel writes `requestedSource`; `RenderingSystem` and `GamePanel` write `view.captureRequested` for their respective views; `RenderPipeline::Execute` gates BeginCapture/sink/finalize on `view.captureRequested` instead of `view.emitImGuiPass`. `capturedSource` snapshot at finalize keeps the active overlay pinned to the right viewport across user toggles.

- **`FrameDebugger::lastRecaptureFrameIndex`** — frame index of the most recent auto-recapture trigger; gated to 6-frame minimum interval (~10 Hz at 60 fps).

- **Per-draw replay dispatch** — `ReplayPassUpToDraw` is now a small dispatcher keyed on `pass.name`; existing GeometryPass body extracted into `ReplayGeometry`. `ReplayShadow` / `ReplayDepthPrepass` / `ReplaySelectionMask` are stubs (deferred to [#100](https://github.com/Hekbas/Luth/issues/100)). Validity gate via `GetPerDrawPreviewKey()` — callers compute the expected key from `(passIdx, localDrawIdx)` and only use the per-draw preview when the key matches. Unsupported pass types leave the key untouched and callers fall back to pass-archive automatically. Panel and viewport overlay (`GetOverlaySource`) both wired to attempt replay for any Draw selection.

- **EventTree node IDs** — counter-based unique IDs replaced with stable IDs derived from node identity (kind + passIndex / drawIndex / hash(label)). ImGui's open/closed state stays attached to the right node when the tree structure shifts.

---

## Build verification

All commits build clean Debug + Release on Windows MSVC. Pre-existing warnings only (`InspectorPanel.cpp` strncpy, `Editor.cpp` chrono cast, `PostProcessInit.cpp` size_t→uint32_t — none touched).

Smoke tests (user-driven):

- Camera-locked A/B screenshot capture-off vs capture-on (post-A1+A1b): metadata + image coherent for selected pass.
- Continuous WASD/orbit with Scene + Game panels open, source = Scene, GTAOMain selected, 30+ s: editor stays responsive (was freezing at ~5–10 s before G chain).
- Capture across all four play states (Editing/Playing/Paused/Paused+Step): all clean, no validation errors.
- Tree open/close: per-node state survives capture re-builds where node identity is stable.
- Viewport overlay scrub: tracks slider position, switches between Scene and Game viewport based on coupled source.
- Resize Scene viewport during Frozen: archives reallocate at new dims (slot-map dimension-mismatch path), no leaks.

---

## Known issues (deferred to follow-up)

- **[#98](https://github.com/Hekbas/Luth/issues/98)** — compute passes (Frustum Culling group, GTAO trio) appear at end of EventTree instead of in graph order. Code inspection couldn't reproduce; A1b sort code is in place and correct on paper. Possibly a stale-build observation; flagged for user re-verification on a fresh build, otherwise needs runtime debug output to track down.

- **[#99](https://github.com/Hekbas/Luth/issues/99)** — Source = Game capture's GeometryPass shows scene camera, not game camera. Capture-path analysis suggests the right view.camera is used for UBO updates, but the displayed image is wrong. Likely candidates: archive slot collision under Source toggling, panel/viewport descriptor cache picking up the wrong view's archive view-pointer, or replay path using stale `m_CurrentViewResources`.

- **[#100](https://github.com/Hekbas/Luth/issues/100)** — extend per-draw replay to Shadow / DepthPrepass / SelectionMaskPass. Dispatch infrastructure shipped (commit `21b499e`); per-pass replay bodies are stubs. Each is ~100–150 LOC adapted from `ReplayGeometry`. User originally chose full Unity fidelity, but realized cost during execution was bigger than estimated — explicitly deferred to keep v2.8.6 closeable.

---

## Out of scope (deliberately)

- ImGuiPass per-draw replay — ImGui draws are user-data-dependent and not meaningfully replayable.
- Per-dispatch archiving for compute passes — Luth's compute passes are single-dispatch each.
- Animation preview UX → `animation-quick-pass` (#93).
- Tree UX overhaul beyond F's stable-ID fix — separate effort if pursued.
- `A2: drop SetSerialize + add metadata mutex` — substantially mitigated by `4bb6100`; not worth its own follow-up.
