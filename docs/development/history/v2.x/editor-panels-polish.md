# v2.9.7 — editor-panels-polish

**Date:** 2026-05-04
**Commits:** 18 (on `feat/editor-panels-polish`)
**Issue:** [#116](https://github.com/Hekbas/Luth/issues/116)
**Series:** AAA editor rework, effort 8 of 9

---

## Overview

Six panel-UX improvements before the inspector polish pass: reusable
button-group widgets dedup the Scene toolbar; ProjectPanel + ResourcePanel
gain `ImGuiListClipper` (and ResourcePanel a dirty-tracked rebuild cache);
HierarchyPanel gets a per-row visibility eye; new `Edit > Preferences`
window with Unity-style two-pane layout + cross-section search; new
`Window` menu with persisted per-panel visibility; Edit menu adds
Delete + Duplicate.

Tag-only release. The series milestone Release stays reserved for
`editor-workspaces` (v2.9.9).

The effort shipped in two halves: the initial sub-tasks A–H landed first,
then a smoke-test surfaced enough issues that the wrap-up was deferred.
The second half (J–P) delivered bug fixes, the SplitToggleButton widget,
the full Unity-style toolbar reorg, the Preferences refactor, and the
ResourcePanel cache.

Cut/Copy/Paste in the Edit menu was deliberately deferred — no
`EditorClipboard` subsystem exists, and adding one would have pushed
v2.9.7 from M to L. The menu items are simply absent (not stubbed
disabled) so the absence is honest.

---

## Sub-Tasks

### Initial pass (A–H)

| # | Sub-task | Commit |
|---|---|---|
| A | NEW `widgets/ButtonGroup.{h,cpp}` — `SegmentedButton` + `IconToggleGroup` + `IconToggleButton`; subsumes the active-state push pattern duplicated in ScenePanel | [`ef2de40`](../../../../commit/ef2de40) |
| B | `Panel::m_Open` foundation — persistent user-choice bool separate from per-frame `m_Visible`; new `BeginWindow(name, bool*, flags)` overload | [`cf522b1`](../../../../commit/cf522b1) |
| C | ScenePanel toolbar v1 — gizmo `IconToggleGroup`, render-mode `SegmentedButton`, controls overlay `IconToggleButton` (later replaced by N) | [`6fc1c10`](../../../../commit/6fc1c10) |
| D | HierarchyPanel per-row visibility eye — `SmallButton` overlapping the TreeNode (`AllowItemOverlap`); inactive rows dim text + icon | [`c70ffd7`](../../../../commit/c70ffd7) |
| E | Window menu + visibility persistence — `EditorSettings::panelOpen` map; `Reset Layout` loads `layouts/Default.ini` (auto-saved on first run) | [`98c8f01`](../../../../commit/98c8f01) |
| F | NEW `panels/EditorSettingsWindow.{h,cpp}` v1 — 9 `CollapsingHeader` sections; commit-debounced save (later replaced by O) | [`6c29c63`](../../../../commit/6c29c63) |
| G | Edit menu Delete + Duplicate — wraps `EntityDestroyCommand` / `EntityDuplicateCommand`; `Ctrl+D` shortcut added | [`2e9789b`](../../../../commit/2e9789b) |
| H | ProjectPanel grid → `BeginTable` + `ImGuiListClipper`; flat per-frame `entries` vec across grid/list/search paths | [`3f9910b`](../../../../commit/3f9910b) |

### Test-driven refinements (J–P)

| # | Sub-task | Commit |
|---|---|---|
| J | Per-panel `m_WindowID` ctor assignments — fixes ImGui "10 conflicting ID" warning in the Window menu | [`354bb4a`](../../../../commit/354bb4a) |
| K | Hierarchy eye selection-suppression rewrite — hover-based gating because `IsItemClicked` fires on press while `SmallButton` fires on release | [`fdd12c7`](../../../../commit/fdd12c7) |
| L | Engine fix: `Scene::DuplicateEntity` double-pushed root duplicates into `m_RootEntities` (already added by `CreateEntity`); parented duplicates were left orphaned in roots | [`dffef95`](../../../../commit/dffef95) |
| M | NEW `widgets/ButtonGroup` — `SplitToggleButton` (Unity-style icon-half + chevron-half) | [`17765ae`](../../../../commit/17765ae) |
| N | ScenePanel toolbar full reorg + tri overlay — Grid split, Debug split, Camera split, Gizmos split, controls toggle; tri count moved to top-right viewport overlay | [`8bd6752`](../../../../commit/8bd6752) |
| N+ | Split-button + render-icon refinements — `CARET_DOWN`, wider chevron (0.55→0.75), popup anchored under button, `state=null` icon also opens popup, gizmo split is now a master toggle, GLOBE/EARTH/CIRCLE/HALF_STROKE icons | [`3a5e9cb`](../../../../commit/3a5e9cb) |
| O | Preferences two-pane layout + search — centered on engine window; left section list (resizable splitter); right scrollable body; cross-section search filters rows | [`30a159b`](../../../../commit/30a159b) |
| P | ResourcePanel `ImGuiListClipper` around row loop | [`2b6339f`](../../../../commit/2b6339f) |
| P+ | ResourcePanel dirty-tracked rebuild cache — `m_NeedsRebuild` flips on AssetDatabase change, search/filter edits, sort spec dirty | [`ccba1f2`](../../../../commit/ccba1f2) |
| P++ | ResourcePanel live RefCount + auto re-sort — re-reads RefCount per frame; re-sorts only when sorting by Refs and a value moved | [`232c560`](../../../../commit/232c560) |
| Q | Wrap-up: Version.h, history file, ROADMAP | this commit |

---

## Architectural decisions

### `Panel::m_Open` separate from `m_Visible`

`m_Visible` is set per-frame by `Panel::BeginWindow` from `ImGui::Begin`'s
return value — it tracks whether the window is currently displayed
(uncollapsed, focused tab, on-screen). Toggling visibility from the Window
menu requires a *persistent* bool the user controls, separate from the
per-frame ImGui state.

The new `m_Open` is the persistent user choice. Three render-loop sites in
`Editor::Render` skip when `!m_Open` (gather, snapshot assembly, draw).
Panels opt into menu-driven visibility by switching from
`BeginWindow(name, flags)` to `BeginWindow(name, &m_Open, flags)` — the
overload passes `&m_Open` as `p_open` so the title-bar X also flips it.
Default `true` so panels added in later versions appear by default rather
than vanishing on first launch.

### Hover-based selection gating in HierarchyPanel

The naive pattern `if (IsItemClicked() && !eyeFired) selection;` fails
because `IsItemClicked` on `TreeNodeEx` fires on **press** while
`SmallButton` fires on **release**. By the time the eye click is
registered, the press has already triggered selection.

Fix: capture `nodeHovered` immediately after `TreeNodeEx`, capture
`eyeHovered` after the eye `SmallButton`, and gate selection on
`nodeHovered && !eyeHovered && IsMouseClicked(0)`. Both items report
hovered when the cursor sits over the overlap region (`AllowItemOverlap`),
so `eyeHovered` correctly suppresses selection.

### `Scene::DuplicateEntity` invariant

`CreateEntity` always pushes to `m_RootEntities` — that's its contract for
fresh entities. `DuplicateEntity` then reparents (or expects the caller
to). Pre-fix, the parented branch left the duplicate sitting in
`m_RootEntities` AND in the new parent's `Children` (so the entity rendered
twice in Hierarchy). The root-level branch pushed AGAIN, also a double.

The fix derives `newParent` early, then `RemoveFromRoots(duplicate)` if
the entity will end up parented (either by this function or by the caller
via `skipParentAddition`). Net invariant: each duplicate appears in
exactly one place.

This was a pre-existing engine bug, dormant until v2.9.7's `Ctrl+D`
shortcut surfaced it (no Duplicate trigger existed in the UI before).

### `EditorSettingsWindow` is NOT a `Panel`

The Preferences window has different lifecycle from dockable panels: it
opens / closes on user demand, doesn't dock, has no Gather phase, and
shouldn't appear in the Window menu (toggling "Preferences" alongside
Hierarchy/Inspector would confuse users). Modeling it as a standalone
class with `static Show / Hide / Draw` matches Unity's Preferences and
Unreal's Editor Preferences pattern.

### Preferences cross-section search

When the search box has content, the left pane is greyed and ALL sections
render with their headers; rows are filtered to those whose label matches
(case-insensitive substring). When search is empty, only the selected
section renders. This matches Unity's behavior — typing "FOV" in
Preferences shows it in context regardless of which section was active.

### `SplitToggleButton` design

Two adjacent ImGui `Button` calls with zero `ItemSpacing` render as one
visual unit. The icon half owns the toggle state; the chevron half opens
a popup. When `state=nullptr` (Camera, Gizmos), the icon-click ALSO opens
the popup so the whole button feels unified. Popup positioning anchors to
`(iconMin.x, chevMax.y)` so it always drops below the button (Unity feel)
rather than at the click location.

The widget lives in `widgets/ButtonGroup` alongside `SegmentedButton` /
`IconToggleGroup` / `IconToggleButton`.

### Gizmo master toggle save/restore

A single master gate would require touching every gizmo render site in
`ViewportOverlays.cpp` + `GizmoController.cpp`. Instead, the master toggle
in the Gizmos split-button is editor-side: when the user turns gizmos OFF,
a static `s_savedGizmoFlags` snapshot captures the per-flag state; when ON
again, the snapshot is restored (or sensible defaults if no snapshot
exists yet). Per-gizmo flags stay editable via the chevron menu — the
master toggle is just a convenience for "hide everything, restore later".

### ProjectPanel `BeginTable` because clipper requires uniform rows

`ImGui::Columns` advances state per cell, not per row, and doesn't give
the clipper a meaningful row-count axis to project onto. `BeginTable`
exposes `TableNextRow` + N `TableNextColumn` per row. Row height is
uniform because `DrawItem` already pins the cursor to `startPos + cellH`,
where `cellH` is `m_ThumbnailSize + lineH * k_MaxNameLines + 2.0f` for
grid mode and `max(20.0f, lineH + 4.0f)` for list mode.

### ResourcePanel: dirty-tracked cache + per-frame dynamic refresh

Initial clipper-only commit (P) reduced row-rendering cost but the
per-frame `PopulateData` walked the entire `AssetDatabase::GetRegistry()`
+ filtered + sorted on every frame — dominating panel cost on large
registries.

The cache (P+) splits this into `RebuildIfDirty` (registry walk + filter
+ sort) and the display loop. `m_NeedsRebuild` flips on:
- `AssetDatabase::AddChangeCallback` (registry add/remove).
- Search input edit.
- Filter checkbox edit.
- Sort spec dirty.

Per-frame cost drops from O(N log N registry walk + sort) to clipper-bound
row render unless inputs change.

`RefreshDynamicData` (P++) runs every frame on the *cached* entries (not
the registry) to re-read `RefCount` from `AssetManager::GetAsset` —
RefCount drifts as shared_ptrs hand out / drop without bumping the
registry callback. Re-sort fires only when the user is sorting by Refs
AND a value actually moved. Cost is O(cached) map lookups, which is
small relative to the original O(registry) walk.

### Render mode icons

Settled on Unity-style sphere variants after iterating with the user:
- Wireframe → `ICON_FA_GLOBE` (lat/long wireframe sphere)
- Shaded Wireframe → `ICON_FA_EARTH_AMERICAS` (textured globe; stubbed
  disabled with "engine support pending" tooltip — `ShadeMode` enum doesn't
  have it yet)
- Unlit → `ICON_FA_CIRCLE` (flat sphere)
- Lit → `ICON_FA_CIRCLE_HALF_STROKE` (half-shaded sphere — the exact
  Unity convention; minor visual collision with the Material asset icon
  in ProjectPanel/Hierarchy but different visual context)

### `Del` shortcut not added to `Editor::ProcessShortcuts`

HierarchyPanel already handles `Del` when the panel is focused. Adding a
global handler would double-fire. The Edit menu's `"Del"` hint accurately
describes the existing focused-panel shortcut. `Ctrl+D` (Duplicate) is
genuinely new and lives in `ProcessShortcuts` because there's no
panel-local handler today.

### Default layout snapshot deferred to first Render

ImGui's dock state isn't populated until panels render at least once.
`Init` runs before any frame, so calling `SaveLayout("Default")` there
would save an empty snapshot. The `s_NeedDefaultLayoutSave` flag is set
in `Init` based on `fs::exists("layouts/Default.ini")` and consumed at
the end of the first Render.

---

## Files modified

| File | Change |
|---|---|
| `luthien/source/luthien/widgets/ButtonGroup.h` | NEW — 4 helpers in `Luth::UI` (Segmented + IconToggleGroup + IconToggleButton + SplitToggleButton) |
| `luthien/source/luthien/widgets/ButtonGroup.cpp` | NEW — active-state push pattern + popup-anchored split button |
| `luthien/source/luthien/widgets/Widgets.h` | Add `#include "luthien/widgets/ButtonGroup.h"` |
| `luthien/source/luthien/Editor.h` | `Panel::m_Open` + 2-arg `BeginWindow` overload + `s_NeedDefaultLayoutSave` flag; `ApplyPersistence` promoted to public |
| `luthien/source/luthien/Editor.cpp` | Render loop guards; Init defers default-layout flag; ApplyPersistence hydrates `m_Open`; SaveSettings mirrors `m_Open` back; end-of-Render Default.ini snapshot; Edit menu Preferences + Duplicate + Delete; new Window menu; `Ctrl+D` shortcut; `EntityCommands.h` + `EditorSelection.h` + `EditorSettingsWindow.h` includes |
| `luthien/source/luthien/EditorSettings.h` | `<unordered_map>` + `panelOpen` + `showTriIndicatorOverlay` + `lastDebugMode` |
| `luthien/source/luthien/EditorSettings.cpp` | JSON load/save for new fields |
| `luthien/source/luthien/panels/ScenePanel.cpp` | Full toolbar reorg (gizmo toggle group + Grid split + transport center + render-mode icons + Debug/Cam/Gizmos splits + overlay); tri overlay top-right; gizmo master save/restore |
| `luthien/source/luthien/panels/HierarchyPanel.cpp` | `AllowItemOverlap`; eye SmallButton; hover-based selection gating; dim disabled rows |
| `luthien/source/luthien/panels/EditorSettingsWindow.h` | NEW — standalone class |
| `luthien/source/luthien/panels/EditorSettingsWindow.cpp` | NEW — two-pane layout, cross-section search, commit-debounced save |
| `luthien/source/luthien/panels/ProjectPanel.cpp` + `.h` | `m_VisibleEntries` flat vec, `BeginTable`, `ImGuiListClipper` |
| `luthien/source/luthien/panels/ResourcePanel.h` | `m_NeedsRebuild` + `RebuildIfDirty` + `RefreshDynamicData` decls |
| `luthien/source/luthien/panels/ResourcePanel.cpp` | Dirty-tracked rebuild + per-frame RefCount refresh + `ImGuiListClipper` row loop |
| `luthien/source/luthien/panels/{Hierarchy,Inspector,Project,Resource,Scene,Game,Render,Profiler,History}Panel.cpp` | `m_WindowID = "<Name>"` in ctor |
| `luthien/source/luthien/panels/FrameDebuggerPanel.h` | Inline ctor sets `m_WindowID` |
| `luth/source/luth/scene/Scene.cpp` | `DuplicateEntity` no longer double-pushes; parented duplicates leave the root list correctly |
| `luth/source/luth/core/Version.h` | `VERSION_PATCH` 6 → 7 |

---

## Out of scope (deliberate)

- **Cut / Copy / Paste commands** — needs `EditorClipboard` + 3 new commands; deferred to keep v2.9.7 within the M budget.
- **TransportBtn dedup** — single-use lambda in ScenePanel, not worth a helper.
- **DockBuilder default layout** — first-run `SaveLayout("Default")` snapshot is sufficient for `Reset Layout`.
- **Master gizmo flag in engine** — the editor-side save/restore in ScenePanel covers the UX without touching `ViewportOverlays.cpp`. A proper engine-side `showAllGizmos` gate is a v2.9.x candidate if the snapshot pattern feels brittle.
- **Persistent `m_VisibleEntries` member in ProjectPanel** — per-frame rebuild is cheap relative to the saved DrawItem work. (ResourcePanel got the cache because the registry walk was the bottleneck there, not the row render.)
- **ShadedWireframe render mode** — button stubbed disabled; engine `ShadeMode` enum doesn't have it. Slot for a follow-up.
- **Engine-side AssetChangedSignal for live AssetManager refcounts** — the per-frame `RefreshDynamicData` walk is bounded to cached entries; it's cheap enough that adding a signal isn't worth it.

---

## Verification

Manual smoke test in editor (no automated harness for ImGui interactions):

| # | Case | Result |
|---|---|---|
| 1 | Window menu lists 11 distinct panel names; toggle works | OK |
| 2 | Hierarchy eye toggles entity active without selecting; row dims | OK |
| 3 | Ctrl+D creates exactly one duplicate (root and parented entities) | OK |
| 4 | Scene toolbar layout: gizmos | grid | transport-centered | render-modes | debug | cam | gizmos | overlay | OK |
| 5 | Grid split: icon toggles `showGrid`; chevron opens grid sub-settings popup | OK |
| 6 | Debug split: icon toggles current<->lastDebugMode; chevron radio over Normals/EntityID | OK |
| 7 | Camera split: icon-click opens popup (unified); chevron does the same | OK |
| 8 | Gizmos split: icon hides ALL gizmos and restores them on re-toggle | OK |
| 9 | Tri indicator overlay: top-right of viewport; togglable from Gizmos chevron | OK |
| 10 | Edit > Preferences: opens centered, two-pane, search filters across sections | OK |
| 11 | ResourcePanel: filter / search / sort all rebuild instantly; no per-frame walk | OK |
| 12 | ResourcePanel: sorting by Refs auto-resorts as load/unload changes refcounts | OK |

### Tracy (after vs before)

- ProjectPanel `OnDraw` cost on a 200+ asset folder — clipper-bounded.
- ResourcePanel `OnDraw` cost — drops to clipper render only when nothing changes.
