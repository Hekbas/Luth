# v2.7.5 — editor-scene-panel-slim

**Date:** 2026-04-23
**Commits:** 4 (on `refactor/editor-scene-panel-slim`)
**Issue:** [#90](https://github.com/Hekbas/Luth/issues/90)

---

## Overview

Sixth and final epic of the post-v2.6 editor architecture review. `ScenePanel.cpp` had accumulated five responsibilities (1001 LOC): toolbar UI, viewport-texture sizing + resize plumbing, ImGuizmo manipulator state, four world-space overlays (bone / light / camera / AABB) with their own projection-and-clipping helpers, and picking/selection dispatch. Three of those ship out to dedicated collaborators under a new `luthien/source/luthien/viewport/` folder — `ViewportRenderer`, `GizmoController`, `ViewportOverlays`. `ScenePanel` is left as a toolbar + picking + camera-control orchestrator.

`ScenePanel.cpp` shrinks 1001 → 443 LOC (−56%); 3 new files under `viewport/` total ~575 LOC. Pure refactor, no behavior change.

With this, the post-v2.6 editor-review series (EE1–EE6) is complete.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | Extract `ViewportRenderer` (viewport size/bounds/focus/hover + scene-texture DS lifetime + per-frame resize dispatch) | [`8dfcc5a`](../../../../commit/8dfcc5a) |
| B | Extract `GizmoController` (gizmo tool state + ImGuizmo manipulator + drag-undo coalesce + Q/W/E/R shortcuts + icon hit-test latch) | [`ebbbf14`](../../../../commit/ebbbf14) |
| C | Extract `ViewportOverlays` (bone / light / camera / AABB overlays + 5 projection-and-clipping helpers) | [`d49bfbd`](../../../../commit/d49bfbd) |
| D | Version bump + history + roadmap | this commit |

---

## Key Changes

- **New folder** — `luthien/source/luthien/viewport/` (sibling of `panels/`, `inspectors/`, `widgets/`, `commands/`). Holds the three new classes; premake `source/**` glob picks them up.

- **`ViewportRenderer`** owns viewport sizing, bounds, focus/hover, and the `VkDescriptorSet` lifetime for the scene-color texture. Public API is `BeginViewport()` (detects size change from `ImGui::GetContentRegionAvail`, enqueues `RenderResizeEvent`, snapshots bounds from `GetCursorScreenPos`), `DrawSceneTexture(RenderingSystem*)` (DS rebind + `ImGui::Image` + post-image `IsWindowFocused`/`IsWindowHovered` sampling), `SetSize(u32,u32)` (called from the panel's resize handler), and the four getters (`GetSize`, `GetBounds`, `IsFocused`, `IsHovered`). Dtor pushes the DS onto `VulkanContext::PushDeletion`.

- **`GizmoController`** owns `m_Operation` (ImGuizmo op or −1), `m_ShowTransformGizmo`, drag-coalesce state (`m_WasUsing`, `m_StartPos/Rot/Scale`), and the icon-click latch (`m_IconClicked` / `m_IconEntity`). Public API: `GetOperation`/`SetOperation`, `IsTransformGizmoVisible`/`SetTransformGizmoVisible`/`GetTransformGizmoVisibleRef` (the `bool*` variant feeds `ImGui::Checkbox`), `ResetFrameState` (clears the icon latch, called once per frame at the top of `OnRender`), `DrawManipulator(view, proj, bounds, size, selected, scene, isFocused, cameraFlying)`, `DrawGizmoIcon(...)`, and `WasIconClicked`/`IconEntity` for the panel's icon-wins-over-pick logic. The Q/W/E/R shortcut block is folded into `DrawManipulator`'s tail (preserving pre-refactor behavior: shortcuts only fire when a selection exists — early return at the top of the method).

- **`ViewportOverlays`** takes `ViewportRenderer&` + `GizmoController&` by const-ref in its ctor (references, not pointers — the panel owns both). Public API is a single `DrawAll(scene, camera, selected)`; four private per-overlay methods (`DrawBoneDebug` / `DrawLights` / `DrawCameras` / `DrawAABBs`) each gate on the appropriate `EditorSettings::showXxxGizmos` flag. Five projection helpers (`ProjectToScreen` / `IsInViewport` / `LightColorToImU32` / `ClipLineToNearPlane` / `DrawClippedLine`) live as private methods — they're consumed only by the overlays and depend on viewport bounds/size + a caller-supplied `EditorCamera`. Light + camera overlays route their sun/bulb/video icons through `m_Gizmo.DrawGizmoIcon(...)` so the click latch stays on the controller.

- **Icon-click routing** — `GizmoController::DrawGizmoIcon` is the single entry point for clickable world-space icons. Callers pass `isHovered` + `hasValidSelection` (the latter gates the `ImGuizmo::IsOver()` guard — stale otherwise when no manipulator is active). The latch state is read back in `ScenePanel::OnRender` after the overlay pass via `m_Gizmo->WasIconClicked()` / `m_Gizmo->IconEntity()` and beats the `PickingSystem` result when both fire in the same frame.

- **Resize-event subscription stays on `ScenePanel`** — the existing handler drives three things in one shot (`RenderingSystem::Resize`, `EditorCamera::SetViewportSize`, `ViewportRenderer::SetSize`). Splitting the subscription would force ordering coordination across two classes; simpler to keep the single subscription and have `ViewportRenderer` expose a `SetSize(u32,u32)` setter that the handler calls as one of the three updates. `ViewportRenderer` itself doesn't subscribe.

- **Class ownership inside `ScenePanel`** — `std::unique_ptr<ViewportRenderer> m_Viewport`, `std::unique_ptr<GizmoController> m_Gizmo`, `std::unique_ptr<ViewportOverlays> m_Overlays`. Ctor's member-init list allocates `m_Viewport` / `m_Gizmo` first, then `m_Overlays(std::make_unique<ViewportOverlays>(*m_Viewport, *m_Gizmo))`. Declaration order in `ScenePanel.h` matches (ctor-init order warnings avoided).

- **`OnRender` per-frame call order** — (1) `m_Gizmo->ResetFrameState()`, (2) toolbar, (3) `m_Viewport->BeginViewport()`, (4) `m_Viewport->DrawSceneTexture(m_RenderingSystem)`, (5) `m_Gizmo->DrawManipulator(...)`, (6) `m_Overlays->DrawAll(m_Context, m_EditorCamera, m_SelectedEntity)`, (7) picking dispatch (reads `m_Gizmo->WasIconClicked()` for icon-wins-over-pick), (8) F / Shift-F camera framing + `m_EditorCamera.OnUpdate(Δt)`, (9) controls overlay (bottom-left keyboard chip — panel-local, not extracted).

- **Kept on `ScenePanel`** — toolbar (gizmo tool buttons / tri-count / shade mode / env dropdown / camera popup / gizmo-visibility popup / overlay toggle), picking dispatch + `PickingSystem` ↔ `EditorSelection` handoff, controls overlay (bottom-left keyboard chip), F / Shift-F camera framing, `EditorCamera` ownership. `ScenePanel.h` accessors `IsViewportFocused()`/`IsViewportHovered()` now forward to `m_Viewport->IsFocused()/IsHovered()`; `GetEditorCamera()` and `Get/SetShowControlsOverlay` unchanged.

- **Drive-by cleanup** — dead `/*void ScenePanel::SetViewportCamera(...)*/` block (commented out long before EE6, no callers) deleted. Two now-unused includes (`luth/renderer/resources/Model.h`, `luth/resources/AssetManager.h`) and the direct Vulkan includes (`vulkan/vulkan.h`, `VulkanTexture.h`, `VulkanContext.h`, `backends/imgui_impl_vulkan.h`) removed from `ScenePanel.{h,cpp}` — they moved with the code that needed them.

- **Panel header cleaner** — `ScenePanel.h` drops 11 private method declarations (DrawGizmos, DrawGizmoIcon, DrawBoneDebugOverlay, DrawLightGizmos, DrawCameraGizmos, DrawAABBGizmos, ProjectToScreen, IsInViewport, LightColorToImU32, ClipLineToNearPlane, DrawClippedLine) and 8 private data members (m_ViewportSize, m_ViewportBounds, m_IsFocused, m_IsHovered, m_SceneDS, m_LastSceneTex, m_GizmoType, m_ShowTransformGizmo, m_WasUsingGizmo, m_GizmoStartPos/Rot/Scale, m_GizmoIconClicked, m_GizmoIconEntity). What's left: `m_Context`, `m_RenderingSystem`, `m_EditorCamera`, the three `unique_ptr` collaborators, `m_SelectedEntity`, `m_ShowControlsOverlay`. 82 → 44 LOC.

---

## Build Verification

- 4 atomic commits on `refactor/editor-scene-panel-slim`; every commit builds Debug x64 clean (no new warnings; pre-existing C4244 `Editor.cpp:400` chrono-to-uint warning untouched).
- Runtime smoke (user-tested): viewport renders, triangle count + shade-mode switch work, resize without flicker or validation, TRS gizmo drag coalesces into a single undo, Q/W/E/R tool shortcuts gated by focus + not-flying, four overlays toggle in/out via gizmo-vis dropdown, sun/bulb/video icon clicks select their entity and beat picking, F / Shift-F camera framing, controls overlay keyboard chip bottom-left.

---

## Next

Post-v2.6 editor-review series complete. Next up: `play-mode` (v2.8.0) — scene snapshot, Editing → Playing → Paused state machine, system-tick gating, game-vs-editor time separation, camera-source toggle. Issue [#66](https://github.com/Hekbas/Luth/issues/66).
