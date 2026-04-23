# v2.7.4 — editor-component-registry

**Date:** 2026-04-23
**Commits:** 6 (on `refactor/editor-component-registry`)
**Issue:** [#89](https://github.com/Hekbas/Luth/issues/89)

---

## Overview

Replace the hand-written 12-arm `DrawComponent<T>` switch in
`InspectorPanel::DrawEntityComponents` and the 70-LOC hand-written
`Add Component` dropdown with a type-erased `ComponentDrawerRegistry`.
One drawer per component file under `inspectors/component_drawers/`;
each registered once at editor init via `Register<T>()`. The inspector
iterates the registry for both rendering and add-menu enumeration.

Every drawer body, command emission, IsDirty flag, and init-value /
dependency quirk is preserved byte-for-byte. `InspectorPanel.cpp`
shrinks 976 → 255 LOC (−73%, well past the ~40% target).

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | Registry skeleton (`ComponentDrawerRegistry.{h,cpp}` + umbrella `RegisterComponentDrawers()`) + wiring from `Editor::InitPanels`/`Shutdown` | [`ceeceb1`](../../../../commit/ceeceb1) |
| B | Pilot port: `PointLight` drawer → `PointLightDrawer.cpp` with a targeted-lookup shim in `InspectorPanel` | [`96720b0`](../../../../commit/96720b0) |
| C | Port remaining 7 non-DEBUG drawers + 4 DEBUG drawers + `InspectorPanel::SetActiveMaterial` handoff | [`997c3c8`](../../../../commit/997c3c8) |
| D | Flip `InspectorPanel` to iterate the registry; delete the `DrawComponent<T>` template + legacy switch + PointLight shim | [`d3d6b8b`](../../../../commit/d3d6b8b) |
| E | Replace `Add Component` menu body with a registry-driven loop gated by `ShowInAddMenu` + `CanAdd` | [`00ad6f9`](../../../../commit/00ad6f9) |
| F | Version bump + history + roadmap | this commit |

---

## Key Changes

- **`ComponentDrawerDescriptor`** (non-templated POD in `s_Drawers`): `Name`, `AddCommandName` (owns the const char* lifetime for `ComponentAddCommand`), `std::type_index Type`, and four `std::function<…(Entity)>` handles — `HasComponent`, `Draw`, `CanAdd`, `OnAdd` — plus `bool Removable` / `bool ShowInAddMenu`.
- **`Register<T>(name, drawFn, opts)`** closes over `T` at template instantiation: builds `HasComponent = [](Entity e){ return e.HasComponent<T>(); }`; wraps `drawFn` inside `UI::BeginCollapsingHeader(name, true, contextMenu) + UI::EndCollapsingHeader()`; the `contextMenu` emits `ComponentRemoveCommand<T>` iff `Removable`; `CanAdd` defaults to `!HasComponent`; `OnAdd` defaults to issuing `ComponentAddCommand<T>(AddCommandName, …)`. `RegisterSimple<T>(name, drawFn)` is the 1-liner for the common case.
- **Canonical order** held by insertion order in `RegisterComponentDrawers.cpp` under a single `#if defined(DEBUG)` block (ID/Parent/Children/WorldTransform above Transform in Debug builds; absent in Release). No per-entry `DebugOnly` flag, no sort; source-order is the manifest.
- **Init-value + dependency quirks preserved** via per-drawer custom `OnAdd` / `CanAdd`:
  - `AnimationDrawer.cpp` — `OnAdd` snapshots `MeshRenderer.ModelUUID` before constructing `Animation(modelUUID)`.
  - `AnimationControllerDrawer.cpp` — `CanAdd` = `HasComponent<Animation>() && !HasComponent<AnimationController>()`; `OnAdd` reads the entity's `Animation(AnimationIndex/Speed/LoopMode)` and seeds a base `BlendLayer` before issuing the add.
- **MeshRenderer → MaterialEditor handoff** — the legacy drawer wrote to a local `activeMaterialUUID` captured by `[&]` from `DrawEntityComponents`; the registry loop can't carry a return value. Fix: new `InspectorPanel::SetActiveMaterial(UUID)` public setter + private `UUID m_ActiveMaterialUUID` reset at the top of each `DrawEntityComponents` call; `MeshRendererDrawer.cpp` calls `Editor::GetPanel<InspectorPanel>()->SetActiveMaterial(...)` at the end of its drawer body. The trailing `m_MaterialEditor.Draw(*mat)` reads the member.
- **`const char*` lifetime** — `ComponentAddCommand<T>` stores the command name as `const char*`. The default `OnAdd` builds `"Add <Name>"` at Register time and stores it as `std::string AddCommandName` on the descriptor; the lambda captures that string by value (settled into the `s_Drawers` slot after push_back, stable thereafter). `s_Drawers.reserve(32)` on first Register sidesteps any vector-realloc hazard.
- **`DrawComponent<T>` template deleted** — the `BeginCollapsingHeader` + Remove-context-menu behavior migrated verbatim into the registry's `Draw` wrapper. The compile-time `std::is_same_v<T, Transform> || std::is_same_v<T, ID>` check for the Remove-enable flag is replaced by per-descriptor `Removable = false` on Transform and ID.
- **DEBUG-only Tag/Parent/Children add-menu entries dropped** — Tag isn't in the registry (rendered as a special non-collapsible row in `DrawEntityComponents`); Parent/Children are hierarchy-managed (auto-added by `SetParent`/`AddChild`). The legacy DEBUG menu had leftover manual-add items for all three; the registry-driven menu simply omits them via `ShowInAddMenu = false`. Transform and ID also get `ShowInAddMenu = false` (Transform is auto-added on entity create; ID is internal). No user-visible regressions in Release.
- **File layout** — `luthien/source/luthien/inspectors/ComponentDrawerRegistry.{h,cpp}` + `component_drawers/` holding `RegisterComponentDrawers.{h,cpp}` (umbrella) + 8 per-component `.cpp`s (Transform/Camera/MeshRenderer/Animation/BoneAttachment/AnimationController/DirectionalLight/PointLight) + 1 `DebugDrawers.cpp` consolidating the 4 DEBUG drawers under a single `#if defined(DEBUG)` guard. Premake `source/**` glob picks them up automatically.
- **Idiom match** — static class + static `std::vector` + static accessors mirrors `SystemRegistry` / `AssetDatabase` / `EditorSelection`. Explicit `RegisterComponentDrawers()` call from `Editor::InitPanels` avoids any static-init-order fiasco.

---

## Build Verification

- 6 atomic commits on `refactor/editor-component-registry`; every commit builds Debug x64 clean (no new warnings; pre-existing C4996 `strncpy` + C4244 RNG-seed warnings untouched).
- Runtime smoke (user-tested): load a scene with an entity carrying every component, edit one property per component and undo/redo each, vector ops on AnimationController.Layers, right-click Remove (Transform + ID disabled), Add-Component menu filters by `CanAdd` (AnimationController hidden until Animation present), Animation add seeds ModelUUID from MeshRenderer, AnimationController add seeds base BlendLayer from current Animation state, scene round-trip identical.

---

## Next

`editor-scene-panel-slim` — extract `GizmoController` + `ViewportRenderer` + `ViewportOverlays` from `ScenePanel`; last epic in the post-v2.6 editor-review series. Or pivot to `play-mode` (v2.8.0) feature work.
