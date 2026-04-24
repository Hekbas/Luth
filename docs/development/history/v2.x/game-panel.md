# v2.8.1 — game-panel

**Date:** 2026-04-24
**Commits:** 12 (on `feat/game-panel`)
**Issue:** [#91](https://github.com/Hekbas/Luth/issues/91)

---

## Overview

Second v2.8 release. Replaces the v2.8.0 scene-camera-override stopgap with a dedicated Game panel that renders through the first `Component::Camera` entity — no editor overlays (grid / outline / gizmos / bone debug / AABB), Unity-style letterbox/pillarbox respecting the camera's aspect ratio. Scene panel goes back to always using the editor camera and keeps every overlay.

What started as a "½-1 day" wiring job opened the full multi-view architecture question: the pipeline had one `GlobalUBO`, one Set 0 descriptor set, and one indirect buffer, all shared — fine when only one view ever rendered per frame, not fine when two views need their own cascades, own GTAO, own cull output, own post-process descriptors. The plan assumed "per-Execute descriptor rebind" would be safe; it isn't (vkUpdateDescriptorSets on a set with in-flight reads is UB). So the bulk of the epic is a pipeline refactor: **per-view `ViewResources` cache, shared primary command buffer, per-view indirect regions**. GamePanel itself is ~150 LOC on top; the rest is renderer plumbing so the second view doesn't alias the first.

Three drive-by fixes for pre-existing bugs that stricter validation in the Vulkan SDK surfaced during testing (depth texture `TRANSFER_SRC` for the frame debugger, explicit `builder.Read(gtaoFinal)` so the RG transitions it before pbr.frag samples it, correct `oldLayout` on the picking barrier).

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| C1 | Remove v2.8.0 scene-camera-override stopgap (`EditorViewportState::playCamera*`, `useEditorCameraInPlay`, App.cpp override, ScenePanel toggle) | [`3ce344a`](../../../../commit/3ce344a) |
| C2 | Parameterize `RenderPipeline::Execute` with `RenderView` (targets + camera + overlay flags) | [`5b687ce`](../../../../commit/5b687ce) |
| C3 | Per-instance `ViewportRenderer::SetOnResize` callback; drop `RenderResizeEvent` bus | [`4da96c0`](../../../../commit/4da96c0) |
| C4 | Initial GamePanel scaffolding (later found to have aliasing + double-present issues — architecture revised in C4.1-C4.3) | [`701d11c`](../../../../commit/701d11c) |
| C4.1 | `ViewResources` struct + per-FrameTargets resource cache | [`230edbb`](../../../../commit/230edbb) |
| C4.2 | Split `Renderer::ExecuteGraph` into `BeginPrimaryCmd` / `RecordGraph` / `EndPrimaryCmdAndSubmit`; per-view indirect regions via `view.viewIndex` | [`9c6fb1b`](../../../../commit/9c6fb1b) |
| C4.3 | `RS::QueueView` API; GamePanel queues its view; LDR layout finalize barrier for non-primary views | [`cff59e5`](../../../../commit/cff59e5) |
| F1 | Don't pre-flip Vulkan Y-axis on GamePanel projection (UpdateGlobalUniforms already applies it) | [`4a325b3`](../../../../commit/4a325b3) |
| F2 | Clear queued views in Frozen+static early return; depth `TRANSFER_SRC`; explicit `builder.Read(gtaoFinal)` in GeometryPass | [`4c001df`](../../../../commit/4c001df) |
| F3 | PickingSystem barrier uses `COLOR_ATTACHMENT_OPTIMAL` source (actual EntityID end-of-frame layout) | [`7fd96f0`](../../../../commit/7fd96f0) |
| F4 | Letterbox GamePanel (`ViewportRenderer::BeginViewport(aspectRatio)`); sync IBL/skybox intensity from EditorSettings | [`563b61d`](../../../../commit/563b61d) |
| W | Sanitize branch comments | [`c224698`](../../../../commit/c224698) |

---

## Key Changes

### Architecture

- **`RenderView`** — per-view input bundle in `luth/renderer/RenderPipeline.h`: `FrameTargets* targets` (non-owning), `CameraParams camera`, `u32 viewIndex`, `bool drawGrid / drawSelectionOutline / emitImGuiPass`. One `RenderView` per visible viewport; `viewIndex` selects the view's slice of the shared indirect buffer.

- **`ViewResources`** — per-view GPU state keyed by `FrameTargets*` pointer in `m_ViewResources` map. Holds the view's `descPool` (owns every descriptor set below — one `vkDestroyDescriptorPool` frees them all on release), `globalUniformBuffer` + `globalDescriptorSet` (Set 0), `gtaoUBOBuffer`, 2 bloom ping-pong textures, 4 GTAO storage textures, 4 bloom/composite descriptor sets, 3 GTAO descriptor sets, outline + grid descriptor sets. Allocated on first `Execute` via `EnsureViewResources`, rebuilt on size change, destroyed on `ReleaseViewResources` (panel dtor) or pipeline shutdown.

- **Shared primary command buffer** — `Renderer::ExecuteGraph` split into three calls (`BeginPrimaryCmd` / `RecordGraph` / `EndPrimaryCmdAndSubmit`) so `RS::Update` records multiple views into one primary cmd buffer with a single submit + present per frame. Old `ExecuteGraph` kept as a convenience that wires all three (used by `ExecuteMinimal` in the frame-debugger Frozen path).

- **`RS::QueueView` API** — editor panels (GamePanel today, PIP / reflection probes later) push a `RenderView` onto `m_QueuedViews` before `RS::Update` runs. Update records queued views first (their LDRs are sampled via `ImGui::Image` by the scene view's ImGui pass), then the scene view (which closes with the ImGui pass + present barrier). Queue is cleared each Update — including the Frozen+static early return path, to prevent unbounded growth during debugger freeze.

- **Per-view indirect regions** — `k_IndirectRegionCount = k_MaxViews * k_IndirectRegionsPerView` (2 × 5 = 10 regions in the shared buffer). Each view owns a disjoint range `[viewIndex * 5, +5)` so cull / shadow / geo / depth draws don't stomp on each other's instance counts. ShadowPass / GeometryPass / DepthPrepass compute `viewBaseRegion = m_CurrentView->viewIndex * k_IndirectRegionsPerView` and offset their `vkCmdDrawIndexedIndirect` calls accordingly; the cull dispatch offsets similarly.

- **`Execute(registry, view, primaryCmd)`** — takes the primary cmd as a `void*`. Caches `m_CurrentView` + `m_CurrentViewResources` at entry, builds the per-view subgraph (cull → shadow × 4 → depth prepass → GTAO × 3 → geo → skybox → bloom → grid? → post-process → outline? → ImGui?), calls `Renderer::RecordGraph`, then emits an explicit `COLOR_ATTACHMENT_OPTIMAL → SHADER_READ_ONLY_OPTIMAL` barrier on the LDR for non-primary views so the scene view's ImGui pass can sample it. Frame-debugger capture + finalize gate on `view.emitImGuiPass` so only the scene view writes capture state (per-view capture is backlog).

- **Per-instance resize callback** — `ViewportRenderer::SetOnResize(std::function<void(u32, u32)>)` replaces the single-subscriber `RenderResizeEvent` bus. ScenePanel's callback resizes `RS::m_SceneTargets` + `EditorCamera`; GamePanel's resizes its own `FrameTargets`. `RenderResizeEvent.h` + `EventCategoryRender` enum entry deleted.

### Editor

- **`GamePanel`** in `luthien/source/luthien/panels/` — `class GamePanel : public Panel` owns `RenderingSystem*`, its own `FrameTargets`, a `ViewportRenderer`. On `OnRender`: looks up the first `<Component::Camera, Component::WorldTransform>` entity, builds `CameraParams` from it (view = `Math::Inverse(xf.Matrix)`, projection from `Math::Perspective`/`Math::Ortho` — left Y-up, UGU applies the Vulkan flip uniformly for every view; IBL/skybox intensities pulled from `EditorSettings`), calls `ViewportRenderer::BeginViewport(camAspect)` for letterbox/pillarbox, queues the view via `RS::QueueView` with `viewIndex=1` / `drawGrid=false` / `drawSelectionOutline=false` / `emitImGuiPass=false`, then draws the LDR texture. Placeholder ("No Camera entity in scene") when absent. Registered in `Editor::InitPanels` right after ScenePanel.

- **`ViewportRenderer::BeginViewport(aspectRatio = 0.0f)`** — aspect = 0 fills the panel (scene behaviour); aspect > 0 computes the largest rect matching that aspect inside the panel content region, centers it (bars fill remainder, picked up by panel background), resizes targets to the inner rect, offsets cursor before `DrawSceneTexture`. Matches Unity's game view.

- **`EditorCamera` convention** — editor camera's `m_ProjectionMatrix` stays Y-up (no pre-flip); `RenderPipeline::UpdateGlobalUniforms` applies `ubo.projection[1][1] *= -1.0f` once per view. GamePanel's `BuildCameraFromScene` follows the same rule (a double-flip bug produced upside-down rendering with inverted winding → backfaces, caught in smoke test, fixed in F1).

### Bugs fixed along the way

- **Pre-existing (surfaced by stricter validation)**:
  - Depth textures (`VKTexture::CreateImage`) missed `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` → frame-debugger archive sink's `vkCmdCopyImage` failed validation on shadow map + scene depth.
  - `GeometryPass` never declared `builder.Read(gtaoFinal)`; `pbr.frag` samples it via Set 0 binding 4 (outside RG visibility), so the RG left it in `LAYOUT_GENERAL` from GTAODenoise and the geometry pass's secondary cmd hit a SHADER_READ mismatch.
  - `PickingSystem`'s pre-copy barrier declared `oldLayout = SHADER_READ_ONLY_OPTIMAL` but EntityID ends every frame in `COLOR_ATTACHMENT_OPTIMAL` (no RG reader).

- **Refactor-induced**:
  - C4 initial implementation tried two `RenderPipeline::Execute` calls per frame → double-submit + double-present + descriptor aliasing (mid-frame `vkUpdateDescriptorSets` on sets with in-flight reads). Redesigned as C4.1-C4.3 (per-view `ViewResources` + shared primary cmd).
  - `GamePanel::BuildCameraFromScene` pre-flipped `projection[1][1] *= -1.0f` (copied from the C1-deleted stopgap); UGU applies the flip too → double flip → upside-down + backfaces.
  - `RS::Update`'s Frozen+static early return skipped `m_QueuedViews.clear()`; GamePanel kept calling `QueueView` each frame, queue grew unbounded, massive FPS spike when the debugger exited Frozen (all views flushed at once).

---

## Known Limitations

- **Shadow 2× cost** — `LightingSystem::UpdateFor` fits CSM cascades per view (~1 ms GPU with game panel open). Backlog: frustum-union fit covering all active views.
- **Frame Debugger scene-only** — `view.emitImGuiPass` gates debugger capture/finalize, so the game panel isn't captured. Backlog: per-view tracked-RT slots.
- **Scene-panel post-process toggle** — Unity-style toggle to disable bloom/tonemap/vignette for lookdev is a separate backlog entry; not in this epic's scope.

---

## Files Touched

| Area | Files |
|------|-------|
| **New** | `luth/renderer/ViewResources.cpp`, `luthien/panels/GamePanel.{h,cpp}` |
| **Deleted** | `luth/events/RenderEvent.h` |
| **Renderer core** | `RenderPipeline.{h,cpp}`, `Renderer.{h,cpp}`, `postprocess/PostProcessInit.cpp`, `passes/AOInit.cpp`, `resources/GlobalUniforms.cpp`, `lighting/IBLInit.cpp`, `debug/FrameDebuggerContext.cpp` |
| **Passes (per-view members → `m_CurrentViewResources->`)** | `AOPass.cpp`, `BloomPass.cpp`, `DepthPrepass.cpp`, `GeometryPass.cpp`, `GridPass.cpp`, `OutlinePass.cpp`, `PostProcessPass.cpp`, `SelectionPass.cpp`, `ShadowPass.cpp`, `SkyboxPass.cpp` |
| **Per-view indirect offsets** | `ShadowPass.cpp`, `GeometryPass.cpp`, `DepthPrepass.cpp` |
| **Drive-by fixes** | `backend/vulkan/VulkanTexture.cpp`, `scene/systems/PickingSystem.cpp`, `passes/GeometryPass.cpp` (builder.Read gtaoFinal) |
| **Scene systems** | `RenderingSystem.{h,cpp}` (QueueView + RecordView + Frozen queue clear) |
| **Editor** | `Editor.cpp` (register GamePanel), `panels/ScenePanel.{h,cpp}` (drop toggle + EventBus), `viewport/ViewportRenderer.{h,cpp}` (ResizeFn + aspect lock) |
| **Stopgap removal** | `luth/core/{App.cpp,EditorHooks.h}`, `luth/events/Event.h`, `luthien/{EditorHooks.cpp,EditorSettings.{h,cpp}}` |
| **Docs** | `docs/development/arch/editor.md`, `ROADMAP.md`, `BACKLOG.md`, this history file |

---

## Build Verification

All 12 commits build Debug x64 clean (Luth.lib + Luthien.lib + Luthien.exe). No new warnings introduced — pre-existing chrono C4244 in `Editor.cpp` and `LNK4006 __NULL_IMPORT_DESCRIPTOR` in Vulkan SDK persist.

Manual smoke passed:
- Scene panel renders identically (grid, outline, bloom, GTAO, shadows, frame-debugger capture).
- GamePanel shows the first Camera entity's view; letterbox/pillarbox respects `Camera.AspectRatio` on resize; moving the camera via ScenePanel gizmo updates GamePanel; IBL/skybox sliders affect both views.
- Delete Camera entity → "No Camera entity in scene" placeholder.
- Dock side-by-side, resize each independently → no cross-talk.
- Play / Pause / Stop cycle → both panels redraw; scene keeps its tint.
- Frame Debugger enable/disable cycle: 0 validation errors; no FPS drop on disable (queue-clear fix).
- Click picking: 0 validation errors (barrier-layout fix).
- `Runtime.exe` boots with a Camera-containing scene.

---

## Next

- **v2.8.2** `frame-debugger-scrub` — unrelated frame-debugger panel polish ([#92](https://github.com/Hekbas/Luth/issues/92)).
- **v2.9.0** `jolt-physics` — rigid bodies + colliders, built on `play-mode` ([#56](https://github.com/Hekbas/Luth/issues/56)).
