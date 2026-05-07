# v2.9.15 — frame-debugger-polish-v2

**Date:** 2026-05-07
**Branch:** `feat/frame-debugger-polish`
**Mode:** B (per-effort tag, tag-only — internal tooling polish)
**Issues:** [#98](https://github.com/Hekbas/Luth/issues/98), [#99](https://github.com/Hekbas/Luth/issues/99), [#100](https://github.com/Hekbas/Luth/issues/100)
**Estimate:** L

---

## Overview

Batch-handles the three frame-debugger issues deferred from v2.8.6 (`frame-debugger-polish`) plus eight drive-by polish items, before the `jolt-physics` series begins. The frame debugger is the inspection tool we'll use during physics-visualization GPU work, so polish before jolt earns compound interest.

Three issues closed:

- **#98** event-tree groups (Frustum Culling, Shadows) appeared at first-occurrence position regardless of their members' graph indices, so compute groups (lowest indices) and graphics passes interleaved out of execution order. Two-pass build with `min-graphPassIndex` per group restores graph order.
- **#99** `Source = Game` capture's per-draw replay rendered against scene targets and scene UBO. Snapshot captured-view metadata at `FinalizeCapture`; replay binds the captured view's descriptor set + targets and uses `viewIndex * k_IndirectRegionsPerView` for the indirect-region offset. Closed for the replay path; pre-existing half-rendered preview artifact (filed separately) is orthogonal.
- **#100** per-draw replay was GeometryPass-only. ReplayShadow / ReplayDepthPrepass / ReplaySelectionMask now ship; live SelectionMaskPass instrumented to emit `CaptureDrawCall` per draw so the slider has a valid range.

Tree-shape research before T2 confirmed Luth's existing cascade-grouping model directly mirrors Unity 6 HDRP (`(RP X: Shadow Maps) → Shadows.Render → Cascade N`). The model was right; only ordering was broken.

---

## Sub-Tasks (commit log)

| # | Commit | Subject |
|---|--------|---------|
| T1 | `cf72492` | fix(framedbg): order event tree by graphPassIndex |
| T2 | `14a4a4f` | feat(framedbg): snapshot captured view + ubo bytes |
| T3 | `fd5e976` | fix(framedbg): replay geometry against captured view |
| T3-fix | `4ee05ed` | fix(framedbg): drop set-0 clobber in geometry replay |
| T5 | `de08775` | feat(framedbg): auto-exit when captured view closes |
| T6 | `410b5b7` | feat(framedbg): per-cascade shadow per-draw replay |
| T7 | `2ecbc09` | feat(framedbg): depth-prepass per-draw replay |
| T8 | `eaf6597` | feat(framedbg): selection-mask per-draw replay |
| P4 | `6b77d20` | chore(framedbg): drop [I] prefix in draw labels |
| P5 | `43c7a7b` | feat(framedbg): show render-mode suffix on PBR draws |
| P8 | `5703130` | feat(framedbg): group GTAO passes in event tree |
| P7 | `aa92eab` | feat(framedbg): cascade label shows splits range |
| P3 | `d43c781` | feat(framedbg): tag empty Pass/Cascade nodes "(no draws)" |
| P1 | `9a744ea` | fix(framedbg): free stale archive slots at FinalizeCapture |
| P6 | `f0248e8` | feat(framedbg): recapture on IBL/skybox intensity changes |
| P2 | `3538d26` | feat(framedbg): show primary output + extra-target count |

T4 (archive-path audit) was planned as instrumentation-then-decide but skipped after T3 confirmed the bug was on the replay path — the archive path is correctly populated by per-view-isolated descriptor sets.

T3-fix is a quick correction landed in the same session: the original T3 rewrote Set 0 binding 0 of the captured view's descriptor set via `vkUpdateDescriptorSets` while the set was bound in a still-pending pre-Frozen cmd buffer. Set 0 has no `UPDATE_AFTER_BIND` flag, so the validation layer flagged the write and the driver hung on the subsequent `vkQueueSubmit`. Fix: drop the rewrite. Justification: in Frozen state the live RG halts, so `GPUTaggedPageAllocator::FreeTag` doesn't advance, so the capture-time UBO region the descriptor points at stays alive. The pre-T3 code relied on the same invariant.

---

## Investigation arcs

### #98 ordering — first-encounter vs min-index

The v2.8.6 `FinalizeCapture` sort by `graphPassIndex` is correct on paper and verified working at the post-sort point — `capturedFrame.passes` lands in graph order. The bug was downstream in `BuildEventTree`: it created groups (`Shadows`, `Frustum Culling`) at the *first occurrence* of a member pass, then appended further members under that group. Subsequent ungrouped passes got pushed into root in encounter order. So if `FrustumCull.Cam` (graphIdx=0) was the first compute pass in `passes`, the `Frustum Culling` group landed at root[0] — but the `Shadows` group, created on first cascade encounter (graphIdx≈5), landed at root[1] only because that cascade was the second *non-grouped-yet* pass encountered. Across captures with different cull counts or different culled-pass sets, the relative order of the two groups vs their ungrouped peers became non-deterministic.

T1 introduces a two-pass build with `Entry { node, sortKey }`. Pass 1 classifies into root / Shadows / FrustumCull / GTAO buckets and updates each group's `sortKey` to the running min `graphPassIndex` over its members. Pass 2 `stable_sort`s entries by `sortKey` and pushes to root. Group nodes now appear at the position of their earliest member, not their first encounter.

### #99 — captured-view vs current-view

Three sins compound:
- `ReplayGeometry` reads `m_Pipeline.GetSceneTargets()` for SceneColor/SceneDepth/EntityID — always the editor scene's targets, regardless of which view captured.
- `ReplayGeometry` binds `rp.GetCurrentViewResources()->globalDescriptorSet[slot]` — `m_CurrentViewResources` is set by `RenderPipeline::Execute(view)` and points at the LAST view to run. With the editor's typical scene-after-game ordering, this is the scene view at end of frame.
- The indirect-region offset hardcodes `viewBaseRegion = 0` (scene), so for `Source = Game` (viewIndex=1) draws get reads from the wrong cull-dispatch output.

T2 plumbs a `CapturedViewState` snapshot at `FinalizeCapture`: pointer to captured `FrameTargets`, an identity token (`u64`) minted on `EnsureViewResources`, viewIndex, dims. T3 replaces the three sins with: targets from `cf.capturedView.targets`, set from `m_Pipeline.GetViewResources(targets)->globalDescriptorSet`, and `viewBaseRegion = cf.capturedView.viewIndex * k_IndirectRegionsPerView`.

The original T3 also tried to rewrite the captured view's binding 0 with snapshot UBO bytes (defending against `FreeTag` recycling the capture-time region). This created the UAB-vs-pending-cmd-buffer hazard called out in Agent D's planning critique. T3-fix drops the rewrite — in Frozen state the live RG halts, so `FreeTag` doesn't advance, so the capture-time region survives.

A pre-existing artifact (left half of the per-draw GeometryPass preview renders black; right half shows partial Bistro geometry) reproduces on plain `main` and is unrelated to this effort. Filed as a separate task.

### #100 — three replay paths sharing infrastructure

`ReplayGeometry`'s 5-phase shape (prep barriers → BeginRendering → bind+draw → copy/blit → restore) translates cleanly to Shadow/DepthPrepass/SelectionMask with per-pass tweaks:

- **ReplayShadow** writes through the live cascade-layer view of `m_ShadowMap`. Safe in Frozen — the live RG halts, so no race; on un-Freeze the next live ShadowPass clear-loads the cascade. Uses `cf.lightSpaceMatrix[cascadeIdx]` via push-constant + the cascade-region indirect formula `(viewBaseRegion + cascadeIdx + 1) * stride + gpuObjectIndex`. Tonemaps depth → `m_DepthPreviewImage` (RGBA8, has `COLOR_ATTACHMENT_BIT`) via the existing `depthPipeline`, then `vkCmdBlitImage2` with format conversion to `m_PerDrawPreviewImage` (RGBA16F). Cascade-range push-constants (`cascadeSplitsViewZ[i-1]`..`[i]`) give each cascade a sensible contrast band.
- **ReplayDepthPrepass** is the simplest: opaque-list-only, captured `SceneDepth` as the only attachment, indirect uses the camera region (`viewBaseRegion * stride + gpuObjectIndex`), tonemap with main-camera near/far (0.1..1000m).
- **ReplaySelectionMask** required a capture-side change. The live `SelectionMaskPass` only called `BeginCapturePass`/`EndCapturePass`, never `CaptureDrawCall` — `pass.drawCallCount` was always zero, so the slider had no range. T8's first edit threads `CaptureDrawCall` per issued draw. Replay then uses the new `EditorOverlaysSubsystem` accessors for `SelectionMaskPipeline` / `SelectionMaskSkinnedPipeline`, filters live drawList by current selection set (snapshotted selection deferred — fine for the per-draw scrub, replay reflects current editor selection), uses 5 sets (no Set 5), per-draw `ObjectPushConstants`, and `VK_FILTER_NEAREST` blit to preserve mask edges.

All three share the `ValidateCapturedView` guard from T5 — every Replay path early-returns + auto-exits + surfaces a notice via `EditorHooks::OnFrameDebuggerNotice` when the captured view's `FrameTargets` is no longer in `m_ViewResources` or its identity token has changed.

### Drive-by polish

Eight items, one commit each:

- **P1** archive-slot stale-entry GC. Toggling `view.drawSelectionOutline` or `view.drawGrid` mid-capture session changed the pass-write set; entries from a prior capture's `SelectionMaskPass` lingered in `m_ArchiveSlotMap` until `ExitCapture`. Now tracked per-capture in `m_ArchiveSlotInUseThisCapture`; `FinalizeCapture` walks the slot map and `PushArchiveDeletion`s un-touched entries.
- **P2** pass-detail "Primary Output" row on the inspector table, with `[D]` depth-flag and `(+N)` extra-target count for multi-target passes.
- **P3** `(no draws)` badge on empty Pass/Cascade nodes (e.g. shadow cascades when no models in scene).
- **P4** drop `[I]` prefix from draw labels — every PBR draw is indirect, prefix was noise.
- **P5** `[Op]/[Cu]/[Tr]/[Fa]` render-mode suffix on PBR draw rows.
- **P6** auto-recapture compare key extends to IBL/skybox intensities. Cascade splits / shadow bias would need the lighting system to recompute during Frozen — out of scope.
- **P7** cascade label shows splits range — `Cascade 0  (0.1-15.0 m)`.
- **P8** GTAO group routing (`GTAODepthPrefilter` / `GTAOMain` / `GTAODenoise` nest under "GTAO" group, mirroring Unity's screen-space-effects cluster).

---

## Architectural changes

- **`CapturedFrame::capturedView` (`CapturedViewState`)** — stable identity token (`viewResourcesId`) minted at `EnsureViewResources` first-use, plus `targets` pointer / `viewIndex` / `width` / `height`. Survives across captures via `Clear()` reset. Validated at every Replay entry via `m_Pipeline.HasViewResources(targets, id)` — pointer alone is unsafe under panel-close + reopen-at-same-address.
- **`CapturedFrame::capturedGlobalUboBytes`** — full `GlobalUniforms` byte snapshot from `GlobalSubsystem::UpdateUBO`'s most recent call. Available for future replay-write paths if they need to bind a fresh descriptor pointing at known-alive bytes; current replays trust the Frozen-halt invariant on `FreeTag` and bind the captured view's set as-is.
- **`CapturedFrame::captured{Irradiance,Prefiltered,BRDF,GTAOFinal}` (`shared_ptr<Texture>`)** — keeps capture-time IBL/GTAO textures alive across freeze for self-contained replay (currently unused by the as-is bind path; reserved for the dedicated-replay-set fallback if T3's hazard re-emerges).
- **`CapturedFrame::captured{Ibl,Skybox}Intensity`** — surfaced in P6's recapture compare key.
- **`ViewResources::id`** — `u64` identity token, survives resize, dies with `ReleaseViewResources`. Source: process-local atomic counter starting at 1.
- **`RenderPipeline::HasViewResources(targets, expectedId)` / `GetViewResources(targets)`** — public accessors for replay validation + descriptor-set lookup.
- **`GlobalSubsystem::GetLastUboBytes(out)`** — copies cached `m_LastUboBytes` populated in `UpdateUBO`.
- **`LightingSubsystem::GetShadowPipeline()` / `GetShadowSkinnedPipeline()`** — public accessors for ReplayShadow's pipeline binding.
- **`GeometrySubsystem::GetDepthPrepassPipeline()` / `GetDepthPrepassSkinnedPipeline()`** — public accessors for ReplayDepthPrepass.
- **`EditorOverlaysSubsystem::GetSelectionMaskPipeline()` / `GetSelectionMaskSkinnedPipeline()`** — public accessors for ReplaySelectionMask.
- **`IEditorHooks::OnFrameDebuggerNotice(message)`** — engine→editor notice channel, default empty impl. Currently wired to `LH_CORE_INFO` in `LuthienEditorHooks` — surfaces in console panel; a proper toast widget can replace the impl without touching call sites.
- **`FrameDebugger::m_ArchiveSlotInUseThisCapture` (`unordered_set<string>`)** — populated by `OnPassExecuted` per-capture, drained at `BeginCapture` / `DestroyArchives`, consumed at `FinalizeCapture` to free orphaned entries.
- **`SelectionMaskPass` capture instrumentation** — `CaptureDrawCall` per issued draw enables the per-draw scrub slider for selection mask.

---

## Files modified

**New:**
- `docs/development/history/v2.x/frame-debugger-polish-v2.md` (this file)

**Modified:**
- `luth/source/luth/renderer/debug/FrameDebuggerContext.{h,cpp}` — replay infra, 3 replay bodies, auto-exit guard, ValidateCapturedView helper
- `luth/source/luth/renderer/FrameDebugger.{h,cpp}` — UBO-bytes snapshot site (via subsystem), m_ArchiveSlotInUseThisCapture
- `luth/source/luth/renderer/rendergraph/FrameEventTree.cpp` — two-pass build, GTAO group, [I] drop, render-mode suffix, cascade splits label, no-draws badge
- `luth/source/luth/renderer/rendergraph/FrameCapture.h` — `CapturedViewState`, snapshot fields, `Clear()` extensions
- `luth/source/luth/renderer/RenderPipeline.{h,cpp}` — snapshot site, `HasViewResources` / `GetViewResources` accessors
- `luth/source/luth/renderer/ViewResources.cpp` — `id` minting, `GetViewResources` / `HasViewResources` impls
- `luth/source/luth/renderer/subsystems/GlobalSubsystem.{h,cpp}` — `m_LastUboBytes` cache, `GetLastUboBytes`
- `luth/source/luth/renderer/subsystems/LightingSubsystem.h` — pipeline accessors
- `luth/source/luth/renderer/subsystems/GeometrySubsystem.h` — pipeline accessors
- `luth/source/luth/renderer/subsystems/EditorOverlaysSubsystem.{h,cpp}` — pipeline accessors, CaptureDrawCall in SelectionMaskPass
- `luth/source/luth/scene/systems/RenderingSystem.cpp` — extended CompareKey for IBL intensities
- `luth/source/luth/core/EditorHooks.h` — `OnFrameDebuggerNotice` virtual
- `luth/source/luth/core/Version.h` — bump to 2.9.15
- `luthien/source/luthien/EditorHooks.cpp` — log-based notice impl
- `luthien/source/luthien/panels/FrameDebuggerPanel.cpp` — Primary Output row + multi-target indicator

---

## Build verification

Debug x64 builds clean after every commit. Pre-existing warnings only (`InspectorPanel.cpp` `strncpy`, `Editor.cpp` chrono cast, `Properties.cpp` `sscanf` — none touched).

Smoke tests (user-driven):

- **#98** — capture, expand tree → Frustum Culling group at top, Shadows after, GTAO between DepthPrepass and GeometryPass, ImGuiPass last. Confirmed.
- **#99** — Source = Scene replay shows scene camera (regression check); Source = Game replay shows game camera. Confirmed for the per-draw replay path.
- **No freeze** — slider scrub through GeometryPass draws no longer hangs the editor (fixed in T3-fix).

---

## Known issues (deferred)

- **Pre-existing GeometryPass replay artifact** — the per-draw preview renders only the right portion of the captured scene; left ~40-50% is uniform black. Reproduces on `main` (`3337cce`); predates this effort. Filed as a separate `type: bug` for follow-up. Affects all four replay paths (geometry/shadow/depth-prepass/selection-mask) since they share the same render-into-preview pattern.
- **Pre-existing `BlitArchivedDepthToPreview` UAB violation** — `vkUpdateDescriptorSets` writes binding 0 of `fd.descSet` while it can be bound in a pending cmd buffer (the existing comment incorrectly assumes ImmediateSubmit's fence-wait makes it safe — that handles the same descriptor's prior submit, not other pending uses). Filed separately; same UAB-flag-or-pool-allocation fix as T3-fix's geometry-side change.
- **GTAO settings UBO (Set 0 binding 5) freed-region hazard during long Frozen sessions** — `GTAOSubsystem::UpdateUBO`'s tagged-heap region for the captured view's slot is freed at `FreeTag(N-2)`. In Frozen the live RG halts and `FreeTag` doesn't advance, so this is fine for the durations users typically scrub (seconds); a session that idles in Frozen for many frames could drift. The dedicated replay descriptor set design considered in planning would eliminate this; deferred.
- **ReplaySelectionMask uses live selection** — capture-time selection set isn't snapshotted to `CapturedFrame`; user selection changes mid-Freeze affect what the replay shows. Acceptable trade-off for the per-draw scrub UX.

---

## Out of scope (deliberately)

- **Dedicated replay descriptor set** — Agent D's planning critique recommended a separate `m_ReplayGlobalSet` allocated from a private pool, rewritten per replay, eliminating the UAB hazard entirely. T3's clobber-rewrite hit that hazard and was dropped in T3-fix; the as-is bind path works because Frozen halts `FreeTag`. If the hazard re-emerges (e.g. live RG running again in Frozen for some future feature), the dedicated set is the cleanup.
- **Capture-time `selectedEntities` snapshot for SelectionMask replay** — small feature, deferred.
- **Cascade splits / shadow bias in auto-recapture compare** — would require lighting system to recompute during Frozen.
- **Toast widget UI** — current `OnFrameDebuggerNotice` impl logs to console; a floating toast widget is future polish.
