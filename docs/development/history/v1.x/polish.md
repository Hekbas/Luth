Here is the revised documentation with all the "conversation" framing removed to read like standard release notes or project documentation.

# Phase 6 — Polish, Bug Fixes & Editor QOL

**Date:** 2026-03-25
**Commits:** 8

---

## Overview

After completing Phase 5 (PBR rendering pipeline), Phase 6 addressed accumulated bugs, editor UX gaps, and missing tooling.

---

## Critical Bug Fixes

### TransformSystem job dispatch (root cause)
The `JobSystem::Dispatch(count, groupSize)` creates `ceil(count/groupSize)` groups, each assigned `JobIndex = group * groupSize`. The transform job only processed `entities[args.jobIndex]` — a single entity per group. With < 64 entities and groupSize=64, only entity[0] was ever updated. **Fix:** job now iterates `[jobIndex, jobIndex+groupSize)`.

### Other fixes
* **Alpha cutoff:** Only applied to `Cutout` render mode; Opaque/Transparent/Fade get `alphaCutoff = 0.0f`
* **Shadow distance:** Replaced hardcoded 50-unit ortho frustum with configurable `ShadowOrthoSize`/`ShadowDistance` on `DirectionalLight` (default 200), exposed in inspector and serialized
* **Stale selection:** Inspector clears selection silently instead of showing red "Asset not found"
* **Vec3 overflow:** `PushMultiItemsWidths` now accounts for colored axis button widths
* **Texture format UB:** `.c_str()` fix for varargs string

**Files:** `TransformSystem.h`, `Material.cpp`, `RenderingSystem.cpp/.h`, `Components.h`, `InspectorPanel.cpp`, `UI.cpp`, `TextureEditor.cpp`, `SceneSerializer.cpp`

---

## Editor Theme & Visual Polish

* **Rider dark theme:** New `SetRiderStyle()` preset with JetBrains Rider color palette
* **Font upgrade:** Replaced default font with better editor-suited typeface
* **Hierarchy icons:** Entity-type-aware icons (camera, light, mesh, animated, etc.)
* **Tree lines:** Visual connector lines in hierarchy for parent-child relationships
* **Camera speed:** Adjustable editor camera speed in ScenePanel toolbar
* **.luth icon:** Scene file icon in ProjectPanel

**Files:** `EditorStyle.cpp`, `HierarchyPanel.cpp`, `ScenePanel.cpp/.h`, `ProjectPanel.cpp`

---

## Inspector System Overhaul

* **Texture inspector:** Redesigned layout — settings/info above, large preview below
* **Material inspector:** UV index dropdown, face culling control, color pickers for scalar properties
* **Model inspector:** Import settings section (scale, axis conversion)
* **Shader inspector:** New read-only source view with SPIRV-Cross reflection data
* **Scene inspector:** Entity count, component summary, "Load Scene" button
* **Font inspector:** Font metadata and glyph preview
* **"No importer" fix:** Silent skip for asset types without registered importers

**Files:** `TextureEditor.cpp`, `MaterialEditor.cpp`, `ModelViewer.cpp`, new `ShaderEditor.h/.cpp`, `SceneViewer.h/.cpp`, `FontViewer.h/.cpp`, `InspectorPanel.cpp`, `AssetManager.cpp`

---

## Editor Persistence & Settings

* **EditorSettings:** JSON-based settings file — style preset, camera speeds, last scene, window size
* **Layout save/load:** Named layout presets stored as `.ini` files in `layouts/`
* **Dirty detection:** Inspector property edits and gizmo usage mark scene dirty
* **IBL panel:** Intensity multiplier and environment map controls in RenderPanel

**Files:** `EditorSettings.h/.cpp`, `Editor.cpp`, `InspectorPanel.cpp`, `ScenePanel.cpp`, `RenderPanel.cpp`, `RenderingSystem.h`

---

## Scene QOL

* **Mouse picking:** GPU ID buffer (`R32_UINT`) in geometry pass, async readback from frame N-2
* **Selection outline:** Sobel edge detection on entity ID buffer for colored selection highlight
* **Shade modes:** Lit, Unlit, Wireframe (`VK_POLYGON_MODE_LINE`), Normals — dropdown in ScenePanel toolbar
* **Triangle count:** Collected during draw command assembly, displayed in toolbar

**Files:** `RenderingSystem.cpp/.h`, `pbr.frag`, `ScenePanel.cpp`, new `selectionOutline.frag`

---

## Profiler Rework

* **Tab bar UI:** Overview | CPU | Memory | GPU replacing flat collapsing headers
* **Trim assets fix:** Force trim option, feedback with evicted asset count
* **Visual improvements:** Colored progress bars, min/max/avg annotations, health color coding

**Files:** `ProfilerPanel.cpp`, `AssetManager.cpp/.h`

---

## Additional Editor Enhancements

* **Custom collapsing headers:** `UI::BeginCollapsingHeader` with integrated styling
* **Material inspector layout overhaul:** Unified entity component layout
