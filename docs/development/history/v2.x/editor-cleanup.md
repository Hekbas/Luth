# v2.7.0 — editor-cleanup

**Date:** 2026-04-19
**Commits:** 6 (on `epic/editor-cleanup`)
**Issue:** [#85](https://github.com/Hekbas/Luth/issues/85)

---

## Overview

First epic of the post-v2.6 editor architecture review. A low-risk housekeeping pass across `luthien/` that removes vestigial files, swaps an O(N) container for O(1), makes panel lookup constant-time, decomposes the `Editor::Init()` monolith, and establishes the comment conventions that subsequent editor-review epics (`editor-style-assets`, `editor-widgets-reorg`, `editor-undo-gaps`, `editor-component-registry`, `editor-scene-panel-slim`) will follow.

No runtime behavior change.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | Delete `Command.cpp` (2 stale comment lines) | [`0d08840`](../../../../commit/0d08840) |
| B | Rename `Command.h` → `commands/Commands.h`; rewrite 8 callsite includes | [`4b3ca29`](../../../../commit/4b3ca29) |
| C | `CommandHistory` stacks: `std::vector` → `std::deque` | [`60cb92a`](../../../../commit/60cb92a) |
| D | `Editor::GetPanel<T>()` O(1) via `type_index` map | [`e51d46f`](../../../../commit/e51d46f) |
| E | Decompose `Editor::Init()` into `InitImGui` + `InitPanels` + `ApplyPersistence` | [`365fe26`](../../../../commit/365fe26) |
| F | Comment audit + version bump + history | this commit |

Each sub-task is an atomic refactor; every commit builds Debug x64 clean.

---

## Changes

### Deletions

- `luthien/source/luthien/Command.cpp` — 2 stale comment lines pointing at `EntityCommands.cpp` / `AssetCommands.cpp`. Premake globs `source/luthien/**` so no build-file change.
- `luthien/source/luthien/Command.h` — moved to `commands/Commands.h`.

### `Command.h` → `commands/Commands.h`

Umbrella include relocated next to the sub-headers it aggregates. The old file's "backward-compatible alias" wording was inaccurate (no prior layout in git history) and was dropped. Callers touched by perl bulk rewrite:

- `luthien/source/luthien/CommandHistory.{h,cpp}`
- `luthien/source/luthien/panels/{HierarchyPanel,InspectorPanel,HistoryPanel,ScenePanel}.cpp`
- `luthien/source/luthien/inspectors/MaterialEditor.cpp`

### `CommandHistory` — deque

`CommandHistory::Execute()` enforces `kMaxHistorySize = 100` via FIFO eviction. With `std::vector`, `erase(begin())` is O(N) and runs once per overflow. Switched both `s_UndoStack` and `s_RedoStack` to `std::deque<std::unique_ptr<ICommand>>`; eviction becomes `pop_front()` — O(1). `HistoryPanel`'s `renderCommands` lambda signature updated from `vector&` → `deque&`. Existing `GetUndoStack()` / `GetRedoStack()` accessors' return types updated; all callers use `auto&` so no downstream churn.

### `Editor::GetPanel<T>()` — O(1)

Before: linear scan over `s_Panels` with a `dynamic_cast` per entry. After: `std::unordered_map<std::type_index, Panel*> s_PanelRegistry` populated by `AddPanel()`, keyed by `std::type_index(typeid(*panel))`. Owning `std::vector<unique_ptr<Panel>>` retained for render order + ownership; registry is a non-owning mirror cleared in `Shutdown()`. Every existing `Editor::GetPanel<X>()` callsite returns the same panel, now in a single hash lookup.

### `Editor::Init()` decomposition

884-LOC `Editor.cpp` had a 120-LOC `Init()` mixing ImGui context setup, Vulkan backend init, settings loading, style selection, window colors, panel instantiation, and per-panel settings application. Split into three private helpers:

- `InitImGui(Window*)` — `ImGui::CreateContext` + IO flags + GLFW/Vulkan backend + dedicated descriptor pool + `ImGui_ImplVulkan_Init` + font upload.
- `InitPanels()` — instantiates the 9 panels (hierarchy / inspector / project / resource / scene / render / profiler / frame-debugger / history) and calls `OnInit()` in insertion order.
- `ApplyPersistence()` — `LoadSettings()` + active-style selection + Win32 titlebar colors + per-panel settings application (`EditorCamera::ApplySettings`, `ProjectPanel::SetThumbnailSize`, `ScenePanel::SetShowControlsOverlay`, `RenderingSystem::ReloadSkybox`).

Public `Editor::Init(Window*)` is now a 5-line orchestrator. Stale `// TODO(Phase 4): Remove this immediate rendering` + `// [ARCHITECTURAL DEBT] Phase 1 Bootstrap` block (superseded when ImGui rendering moved to `RenderingSystem::AddImGuiPass`) deleted as part of the decomposition.

### Comment audit

Comment conventions applied across `luthien/` headers and key `.cpp` files:

- **Deleted**: triple-line `// ================` banner separators (replaced with single-line `// ── Section ──` where a heading still adds value), `// [ARCHITECTURAL DEBT] Phase 1 Bootstrap` + `TODO(Phase 4):` block in `Editor.cpp` (superseded), "backward-compatible alias" in `Command.h` (inaccurate), redundant "Cached stats for display" / "Selection" / "Runtime state" per-field banners in panel headers.
- **Normalized**: `///` doxygen in `Bootstrap.h` → `//` (no doxygen generation is configured).
- **Preserved**: WHY/gotcha comments — `ComponentPropertyCommand` pointer-to-member invariant, `ScenePanel`'s per-instance descriptor-set rationale, `FrameDebuggerPanel`'s view-pointer-keyed cache explanation, `CommandHistory`'s merge-coalescing note.
- **Professional tone**: removed "Hack:" and stale "TODO: fix later" phrases; replaced with short declarative lines stating the invariant or constraint.

Files touched: `Editor.{h,cpp}`, `UI.h`, `Bootstrap.h`, `EditorSelection.h`, all `commands/*.h`, all `panels/*.h`, `inspectors/MaterialEditor.h`, plus banner sweeps in `ProjectLauncher.cpp`, `EditorStyle.cpp`, `panels/FrameDebuggerPanel.cpp`, `panels/ProfilerPanel.cpp`, `panels/ScenePanel.cpp`.

---

## Build Verification

- 6 atomic commits on `epic/editor-cleanup`; every commit builds Debug x64 clean with no new warnings.
- Only pre-existing warnings: C4996 `getenv` in `ProjectLauncher.cpp` and C4244 chrono narrowing at `Editor.cpp:414`.
- `grep "luthien/Command.h" luth/ luthien/ runtime/` returns 0 hits — old include path fully replaced.
- Runtime smoke (user-tested): editor launches, all 9 panels render, sample scene loads, property edits undo/redo, gizmo drag coalesces, style switching works, settings persist across restart.

---

## Next

Follow-up epics in this editor review series (each its own issue + `epic/<slug>` branch when picked up):

- `editor-style-assets` — move `StylePreset` data from `EditorStyle.cpp` into JSON assets under `engine/assets/styles/`; add Save/Load UI for user-custom styles.
- `editor-widgets-reorg` — split `UI.{h,cpp}` into `widgets/Properties.{h,cpp}` + `widgets/AssetSlot.{h,cpp}` + `widgets/CollapsingHeader.{h,cpp}` + `widgets/InfoTable.{h,cpp}` + `widgets/TexturePreview.{h,cpp}`; delete `UI.h`/`UI.cpp`.
- `editor-undo-gaps` — wrap the 14 `Editor::MarkDirty()` animation-layer mutations in `InspectorPanel` through a new `VectorElementPropertyCommand`.
- `editor-component-registry` — replace `InspectorPanel::DrawEntityComponents` hand-written switch with a typed `ComponentDrawerRegistry`.
- `editor-scene-panel-slim` — extract `GizmoController` + `ViewportRenderer` + `ViewportOverlays` from `ScenePanel` (1009 LOC → ~400).
