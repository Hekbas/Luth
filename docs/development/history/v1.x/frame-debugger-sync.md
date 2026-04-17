# Phase 14 — Frame Debugger Sync Rework

**Version:** v1.4.0  |  **Date:** 2026-04-17  |  **Epic:** #74  |  **Supersedes:** #31

---

## What Was Built

Reworked the Frame Debugger into a Unity-grade, GPU-true debugging tool. The old live-replay model — re-executing the pipeline up to N draws using the *current* uniforms/cull state, not the captured ones — was the root of a chronic sync bug where the displayed image never matched the selected step. Phase 14 deletes that path entirely and replaces it with archived per-pass images + on-demand per-draw replay.

- **Archive sink** (`IArchiveSink` + `ArchivedImage`) — `RG::RenderGraph::Execute` invokes a sink hook after every non-culled pass; the FrameDebugger sink emits `vkCmdCopyImage` for each tracked render target into a fresh, persistent staging image, restoring the source layout so the RG's compile-time barrier solver stays consistent. Tracked RTs in v1: `SceneColor`, `SceneDepth`, `ShadowMap.C0..C3` (one per cascade — ShadowPass imports per-layer views with names suffixed by cascade index), `LDROutput`, `EntityID`, `BloomAFinal` (~10 archives, ~50–100 MB at 1080p).
- **Frozen-state model** — strict snapshot. The Frozen branch in `RenderingSystem::Update` does NOT rebuild or re-execute the live graph; `m_LDROutput` retains its captured contents and the editor's ScenePanel keeps showing the GPU-true frame. Each Frozen tick bit-compares the current `viewProj` against `captureViewProj` — a mismatch flips the state machine back to `CaptureRequested` and the next frame runs a fresh capture (Unity behavior: frozen on the captured image, auto-refresh on camera move).
- **Hierarchical EventNode tree** — replaces the flat pass→draw list with `Group / Pass / Cascade / Draw` kinds. An explicit prefix registry (`FrustumCull.` → "Frustum Culling", `ShadowPass.C<N>` → "Shadows" with cascade children) keeps grouping deterministic. Built once at `FinalizeCapture` and stored on `CapturedFrame::rootEvent`.
- **Per-draw replay-then-copy** — clicking a `GeometryPass` draw triggers an `ImmediateSubmit` that re-records the pass up to draw N into `m_SceneColor` and copies the result into a persistent RGBA16F preview the panel samples through ImGui. The live UBOs/SSBOs/indirect buffer are byte-stable in Frozen state (no live writers between captures *once* `AnimationSystem` is paused — see bug fix below), so no separate frozen-buffer plumbing is required. Cache is keyed by `(passIdx, localDrawIdx)` and invalidated on every `BeginCapture` / `ExitCapture`.
- **CSM cascade UI** — Cascade nodes in the tree map to per-cascade single-layer depth archives. `BlitArchivedDepthToPreview` linearizes the selected cascade through the existing depth blit pipeline into an RGBA8 preview, using `[prev_split..this_split]` for sensible per-cascade contrast. Detail panel surfaces capture-time `cascadeSplitsViewZ`, `shadowBias`, `shadowNormalBias`, `cascadeTexelSize`, and the full `lightSpaceMatrix[i]` — values stamped from `m_Cached*` at finalize so editing light parameters while frozen doesn't desync the readout.
- **Lifetime safety** — archive teardown deferred via `VulkanContext::PushDeletion` so in-flight ImGui frames sampling archive views can complete before the views/images are freed. Panel descriptor caches keyed by `VkImageView` pointer (not archive index) so recaptures with overlapping indices always trigger fresh `ImGui_ImplVulkan_AddTexture` calls. All archive frees route through `VulkanAllocator::FreeImage` to keep the editor's `MemoryTracker` GPU counter in sync with VMA.

## Bugs Fixed Mid-Phase

- **Cache key collision across captures** — same `(passIdx, drawIdx)` after a recapture meant the second click on the same draw was a cache hit and the panel served stale preview content. Fixed by invalidating `m_PerDrawPreviewKey` on every `BeginCapture` and `ExitCapture`.
- **MemoryTracker drift** — deferred destroy lambda originally called `vmaDestroyImage` directly, bypassing `VulkanAllocator::FreeImage` (which is what fires `MemoryTracker::RecordFree`). VMA freed the GPU memory but the editor's GPU counter only ever saw allocations. Fixed by routing all archive + per-draw + depth-preview destruction through `VulkanAllocator::FreeImage`.
- **Click handler swallowed by right-aligned annotation** — `ImGui::IsItemClicked` was called *after* the `SameLine + TextDisabled` annotation, so it queried the disabled label (unclickable) instead of the tree node. Fixed by capturing `clickedThisNode` immediately after `TreeNodeEx`.
- **Cascade output "no preview"** — initial tracked-RT set registered `"ShadowMap"` but ShadowPass imports per-cascade resources named `"ShadowMap.C<i>"`, so the sink filtered every cascade write out and cascade nodes had `archivedImageIndex = -1`. Fixed by registering all four `ShadowMap.C0..C3` names. `BlitArchivedDepthToPreview` also needed a fallback to `archive.view` when `archive.layers <= 1` because each cascade archive is single-layer (the source is a per-layer view onto the shared 4-layer image), but the EventNode's `archiveLayer` carries the cascade index 0..3 for detail-panel lookups.
- **Animated meshes drifting between draw replays** — `AnimationSystem::Update` ticked every frame regardless of debugger state, so `BoneMatrixBuffer`'s contents changed between consecutive per-draw replays and each draw rendered a different pose. Fixed by early-returning from `AnimationSystem::Update` when `RenderingSystem::GetDebuggerState() == Frozen`. Mirrors Unity's pause-while-inspecting behavior; will fold into a scene-level pause flag once Phase 16 (physics) and Phase 15 (play mode) land.
- **Timings vanish when scene gains its first model** — `m_GPUTimers.Init(16)` was below the live frame's non-culled pass count. With no models, ShadowPass.C0..C3 are dead-pass-culled, total ≤16. Adding one model un-culls them, total >16, and `GPUTimerPool::ReadResults` early-returns `-1` for *every* slot when `passCount > maxPasses`. Bumped capacity to 64 (current frame ≈19 passes; headroom for GTAO etc.).

## Files Modified / Added

**New:**
- `luth/source/luth/renderer/rendergraph/ArchivedImage.{h,cpp}` — staging-image RAII + lazy per-layer view cache
- `luth/source/luth/renderer/rendergraph/IArchiveSink.h` — RG post-pass hook interface
- `luth/source/luth/renderer/rendergraph/FrameEventTree.{h,cpp}` — hierarchical event model

**Modified:**
- `luth/source/luth/renderer/rendergraph/RenderGraph.{h,cpp}` — `SetArchiveSink` + post-pass invocation
- `luth/source/luth/renderer/rendergraph/FrameCapture.h` — `archivedImages`, `passArchives`, `captureViewProj`, `rootEvent`, cascade cache (splits/bias/texel/light-space matrices)
- `luth/source/luth/renderer/FrameDebugger.{h,cpp}` — `IArchiveSink` impl, archive lifecycle, deferred teardown
- `luth/source/luth/scene/systems/RenderingSystem.{h,cpp}` — gut `RenderCapturedFrame` (~350 LoC), rewrite Frozen branch, `FinalizeCapture`, `ReplayPassUpToDraw`, `BlitArchivedDepthToPreview`, `EnsurePerDrawPreviewTexture`, `EnsureDepthPreviewTexture`, GPU timer pool bumped to 64
- `luth/source/luth/scene/systems/AnimationSystem.cpp` — early-return when Frame Debugger is Frozen
- `luth/source/luth/editor/panels/FrameDebuggerPanel.{h,cpp}` — recursive `DrawEventNode`, archive / per-draw / depth preview paths, cascade detail block
- `luth/source/luth/core/Version.h` — bumped to v1.4.0

## Out of Scope (Future Polish)

- Per-draw replay for non-`GeometryPass` passes (`ShadowPass.C<i>`, fullscreen passes). Today the panel falls back to the pass-output archive for those.
- 3D viewport overlay of cascade frustum slices.
- 4-thumbnail strip for A/B cascade comparison (cascade detail panel currently shows one cascade at a time).
- HDR tonemapping for the per-draw preview (raw RGBA16F surfaces, clipping above 1.0 is annotated in the panel).
- Scene-level pause flag — replace the `AnimationSystem` ↔ `RenderingSystem` direct query with a single `Scene::IsPaused()` flag once `PhysicsSystem` / `PlayMode` land (Phases 15–16).
