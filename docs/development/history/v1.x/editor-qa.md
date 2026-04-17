# Phase 6B — Editor QA Triage

**Date:** 2026-03-30
**Commits:** 12

---

## Overview

22-item QA pass across the editor, resolving crashes, adding missing features, and polishing existing panels. 10 items were already implemented and verified. The remaining 12 were implemented across 5 sessions.

---

## Already-Complete Items (Verified)

| # | Item | Location |
|---|------|----------|
| 1 | MaterialEditor `ImGui::EndChild()` crash | `SkipItems` guard at `MaterialEditor.cpp:23-24` |
| 3 | Load last scene on startup | `Editor.cpp:444-453` (load), `Editor.cpp:486` (save UUID) |
| 4 | Camera settings save/load | Full popup in `ScenePanel.cpp`, `ApplySettings`/`SyncToSettings` wired |
| 5 | Controls overlay | `ScenePanel.cpp:233+`, persisted in `EditorSettings` |
| 8 | Toggle entity active state | Checkbox at `InspectorPanel.cpp:57-61` |
| 9 | Compact Unity-style transform UI | `DrawVecControl` XYZ rows |
| 13 | Hierarchy icons + vertical lines | Icons at `HierarchyPanel.cpp:136-141`, lines at `234-261` |
| 14 | Folder icons (empty vs. non-empty) | `ProjectPanel.cpp` distinguishes by child count |
| 17 | Search bar, icon-size slider, breadcrumb | All present in `ProjectPanel` |
| 18 | Left hierarchy lines + folder icons | `ProjectPanel.cpp:207-243` |

---

## Implemented Items

### Item 2 — Editor Colors system (`EditorColors.h`)

Added semantic color constants: `SelectionOutline`, `ErrorRed`, `WarningYellow`, `SuccessGreen`, `InfoBlue`. `RenderingSystem` now reads `EditorColors::SelectionOutline` instead of a hardcoded `{1.0, 0.6, 0.0, 1.0}`.

**Files:** `EditorColors.h`, `RenderingSystem.h/.cpp`, `ScenePanel.cpp`

---

### Item 7 — Skybox HDR asset selector

`EditorSettings` gained a `skyboxPath` field (default `textures/environment.hdr`), serialized to `editor_settings.json`. `RenderingSystem::RecomputeIBL(path)` extracted from the constructor; `SetSkyboxHDR(path)` is now a public hot-swap method. `ScenePanel` exposes an HDR file picker button in the camera settings popup.

**Files:** `EditorSettings.h/.cpp`, `RenderingSystem.h/.cpp`, `ScenePanel.cpp`

---

### Item 6 — Selection outline: children + occluded fade

The stencil pass now collects the selected entity and all recursive children into an SSBO. The outline fragment shader reads the SSBO (primary entity = full alpha, children = 0.7 alpha) and samples the scene depth texture: if the outlined fragment lies behind opaque geometry, alpha is reduced to ~0.3. `ScenePanel` rebuilds the child list on every selection change.

**Files:** `RenderingSystem.h/.cpp`, `outline.frag`, `ScenePanel.cpp`

---

### Item 10 — Hierarchy search: recursive + case-insensitive

`HierarchyPanel::SubtreeMatchesFilter` recursively checks an entity and all descendants. In `DrawEntityNode`, if no subtree matches the filter the node is skipped; if a child matches but the parent does not, the tree node is forced open. Search is case-insensitive via `tolower`.

**Files:** `HierarchyPanel.h/.cpp`

---

### Item 11 — Missing entries in Hierarchy context menu

"Create" submenu additions: **Point Light** (entity + `PointLight` component) under the Light sub-menu, and **Camera** as a top-level item (entity + `Camera` component).

**Files:** `HierarchyPanel.cpp`

---

### Item 12 — Create basic geometry from Hierarchy

Cube, Sphere, and Plane primitives are shipped as `.fbx` assets under `sandbox/assets/models/primitives/` with `.meta` files. The Hierarchy "Create → Mesh" items create an entity with a `MeshRenderer` pointing to the corresponding primitive UUID.

**Files:** `HierarchyPanel.cpp`, `sandbox/assets/models/primitives/`

---

### Item 15 — Project Panel search includes folders

`ProjectPanel::RecursiveSearch` now also matches directory names against the query, pushing matching `DirectoryNode` entries to `m_SearchResults`.

**Files:** `ProjectPanel.cpp`

---

### Item 16 — Scene load eagerly loads materials and textures

`Editor::OpenScene` iterates all `MeshRenderer` components after `SceneSerializer::Load` and calls `AssetManager::LoadAsync` for each model and material UUID. For each loaded material the texture UUIDs are queued as well, eliminating the blank-until-hover problem.

**Files:** `Editor.cpp`

---

### Items 19–22 — Resource Panel overhaul

Four improvements applied together to `ResourcePanel`:

| # | Change |
|---|--------|
| 19 | **Font and Scene filters** — `m_ShowFonts`/`m_ShowScenes` bools, checkboxes in the filter dropdown, filter conditions in `PopulateData` |
| 20 | **Type icons + compact checkboxes** — `GetTypeIcon()` returns a FontAwesome icon per type (cube / image / circle-half-stroke / file-code / font / film); icon rendered in the Type column with `TextColored`; filter checkboxes use `FramePadding(2,2)` |
| 21 | **Column sorting** — `TableGetSortSpecs()` after building the filtered list; `std::sort` by Name/Type (string) or Refs (int) respecting direction; UUID column marked `NoSort` |
| 22 | **Table polish** — `BordersInnerV` (lighter borders); span-all `Selectable` per row wired to `EditorSelection::SelectResource`; `m_SelectedUUID` tracks the active row; hover tooltip on names longer than 24 characters |

**Files:** `ResourcePanel.h/.cpp`
