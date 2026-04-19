# v2.7.2 — editor-widgets-reorg

**Date:** 2026-04-19
**Commits:** 6 (on `epic/editor-widgets-reorg`)
**Issue:** [#87](https://github.com/Hekbas/Luth/issues/87)

---

## Overview

Split `luthien/UI.{h,cpp}` (506 LOC, ambiguous "UI" name) into five focused
files under `luthien/source/luthien/widgets/`, each scoped to one concern. All
symbols stay in `namespace Luth::UI` so the 12 callsites only update their
`#include`. New `widgets/Widgets.h` umbrella keeps the one-include ergonomic
for panels that touch multiple widgets.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | `widgets/CollapsingHeader.{h,cpp}` + `widgets/InfoTable.{h,cpp}` (small standalone widgets) | [`73dc62e`](../../../../commit/73dc62e) |
| B | `widgets/Properties.{h,cpp}` (+ public `PropertyLabel`; drop dead `static PushMultiItemsWidths`) | [`4da2f63`](../../../../commit/4da2f63) |
| C | `widgets/AssetSlot.{h,cpp}` (`PropertyAsset`; reuses `PropertyLabel` from Properties) | [`9723d9f`](../../../../commit/9723d9f) |
| D | `widgets/TexturePreview.{h,cpp}` (Vulkan ImGui descriptor cache); delete `UI.cpp` | [`b3a23aa`](../../../../commit/b3a23aa) |
| E | `widgets/Widgets.h` umbrella; bulk-rewrite `"luthien/UI.h"` → `"luthien/widgets/Widgets.h"` (12 files); delete `UI.h` | [`3ff6177`](../../../../commit/3ff6177) |
| F | Version bump + docs | this commit |

---

## Key Changes

- **Five widget files** under `luthien/source/luthien/widgets/`:
  - `Properties.{h,cpp}` — `BeginProperties`/`EndProperties`, all `Property<T>` overloads (`std::string`/`bool`/`int`/`float`/`Vec2`/`Vec3`/`Vec4`), `PropertyColor` (Vec3/Vec4), `PropertyCombo`, internal `DrawVecControl` with per-axis reset buttons.
  - `AssetSlot.{h,cpp}` — `PropertyAsset` (label + asset-icon button + `ASSET_UUID` drag-drop target with type validation + right-click clear).
  - `CollapsingHeader.{h,cpp}` — custom-drawn header (rounded background, FA caret icon, grip dots) with optional context-menu callback.
  - `InfoTable.{h,cpp}` — 2-column label/value table + variadic `InfoRow(label, fmt, ...)`.
  - `TexturePreview.{h,cpp}` — aspect-fit centered image + `GetTextureID` (Vulkan ImGui descriptor cache, file-static, weak-ptr stale-entry sweep) + `ClearTextureCache` (must run before `ImGui_ImplVulkan_Shutdown`).
- **`PropertyLabel` promoted to public** in `widgets/Properties.h` so `AssetSlot.cpp` can participate in the same row layout without duplicating the `TableNextRow → TextUnformatted → PushItemWidth(-1)` boilerplate.
- **`widgets/Widgets.h` umbrella** re-includes the five widget headers with a one-line note pointing to per-widget includes for callers that only need one.
- **Drive-by:** dead `static PushMultiItemsWidths` in old `UI.cpp` deleted (real callsite uses `ImGui::PushMultiItemsWidths`, not the file-static one).
- **`Editor::Shutdown`** still calls `UI::ClearTextureCache()` before `ImGui_ImplVulkan_Shutdown` — the symbol now resolves through the umbrella include.

---

## Build Verification

- 6 atomic commits on `epic/editor-widgets-reorg`; every commit builds Debug x64 clean.
- Runtime smoke (user-tested): editor launches, all panels (Hierarchy, Inspector, Scene, Project, Profiler, FrameDebugger, RenderPanel) render; entity inspector shows every component drawer; drag-drop of model/texture/material/shader assets works; no Vulkan validation warnings on shutdown.

---

## Next

`editor-undo-gaps` — wrap the 14 `Editor::MarkDirty()` callsites in `InspectorPanel.cpp` (animation layer Speed/Weight/Loop/ClipIndex) through a new `VectorElementPropertyCommand`, so those edits go through `CommandHistory` and survive undo/redo.
