# Phase 10 — Undo/Redo History System

**Date:** 2026-04-09
**Commits:** 4

---

## Overview

Command-pattern undo/redo system covering ~58 mutation callsites across InspectorPanel, HierarchyPanel, ScenePanel, and MaterialEditor. All entity references use UUID-based resolution to survive entity destroy/recreate cycles.

---

## New Files

| File | Purpose |
|------|---------|
| `editor/Command.h` | ICommand interface + 14 command types (5 template, 9 concrete) |
| `editor/Command.cpp` | Non-template command implementations + CommandUtil serialization helpers |
| `editor/CommandHistory.h` | Static singleton: undo/redo stacks, compound recording, stack accessors |
| `editor/CommandHistory.cpp` | Execute/Undo/Redo logic, merge support, 100-entry cap |
| `editor/panels/HistoryPanel.h` | Debug panel class declaration |
| `editor/panels/HistoryPanel.cpp` | Timeline-style undo/redo stack visualization |

## Modified Files

| File | Change |
|------|--------|
| `scene/Scene.h/.cpp` | Added `FindEntityByUUID(UUID)`, made `AddToRoots()`/`RemoveFromRoots()` public |
| `editor/Editor.cpp` | Ctrl+Z/Y/Shift+Z shortcuts, Edit menu, `CommandHistory::Clear()` on scene transitions, HistoryPanel registration |
| `editor/panels/InspectorPanel.cpp` | ~30 callsites: all property edits, component add/remove, entity rename |
| `editor/panels/HierarchyPanel.cpp` | ~15 callsites: entity create/delete, reparent, reorder, model instantiate |
| `editor/panels/ScenePanel.h/.cpp` | Gizmo drag coalescing via `ImGuizmo::IsUsing()` edge detection |
| `editor/inspectors/MaterialEditor.h/.cpp` | Material undo via JSON snapshot at debounce boundary |

---

## Command Types

| Command | Template | Description |
|---------|----------|-------------|
| ComponentPropertyCommand\<C, T\> | Yes | Change one member of component C via pointer-to-member |
| ComponentAddCommand\<T\> | Yes | Add component T to entity |
| ComponentRemoveCommand\<T\> | Yes | Remove component T (saves value for undo) |
| GizmoTransformCommand | No | Coalesced gizmo drag (start/end transform) |
| EntityCreateCommand | No | Create entity with optional parent |
| EntityDestroyCommand | No | Destroy entity subtree (JSON snapshot for undo) |
| EntityRenameCommand | No | Rename entity |
| EntityReparentCommand | No | Reparent entity (preserves sibling order) |
| EntityReorderCommand | No | Reorder entity within siblings |
| EntityDuplicateCommand | No | Duplicate entity subtree |
| ModelInstantiateCommand | No | Instantiate model (root + mesh children + bones) |
| MaterialSnapshotCommand | No | Full material JSON snapshot |
| CompoundCommand | No | Groups multiple commands as single undo unit |

---

## Key Design Decisions

### UUID-based entity resolution

All commands store `UUID` instead of raw `entt::entity` handles. Entity handles are volatile in EnTT — destroying and recreating an entity (via undo/redo of delete) produces a new handle. Commands resolve entities via `Scene::FindEntityByUUID()` at execution time.

**Three crash fixes** were needed to catch all cases:
1. `ComponentPropertyCommand`, `ComponentAddCommand`, `ComponentRemoveCommand` — crashed on redo of compound entity-creation commands (Create Directional Light → Ctrl+Z → Ctrl+Y)
2. `GizmoTransformCommand`, `EntityRenameCommand` — crashed when undoing a transform after undo-deleting the entity

### Pointer-to-member for property commands

`ComponentPropertyCommand<C, T>` uses `T C::*member` to address specific component fields. This avoids storing raw pointers (unsafe with EnTT pool relocations) and avoids per-property command classes.

The `IsDirty` flag is set automatically via C++20 `requires`:
```cpp
if constexpr (requires(C c) { c.IsDirty = true; }) {
    comp.IsDirty = true;
}
```

### Gizmo drag coalescing

ImGuizmo applies transforms every frame during a drag. A single command is pushed only at drag end, detected via `ImGuizmo::IsUsing()` rising/falling edge in ScenePanel.

### Material undo via JSON snapshot

Material properties use a debounce timer (already existed for save). On first modification, a JSON snapshot is captured. When the debounce fires, a `MaterialSnapshotCommand` is pushed with before/after states.

### Compound commands

`BeginCompound(name)` / `EndCompound()` bracket multi-step operations (e.g., Create Camera = EntityCreate + ComponentAdd). The compound is stored as a single undo unit.

### Known limitations

- **AnimationController layer properties:** Pointer-to-member can't address vector elements. Left as `Editor::MarkDirty()`.
- **Entity active state:** Stored on Entity wrapper (not in registry), making undo unreliable. Left as `Editor::MarkDirty()`.

---

## HistoryPanel (Debug UI)

Timeline-style panel showing the full undo/redo stack:
- Header bar: Undo/Redo/Clear buttons with tooltips
- Continuous vertical timeline with color-coded lines (green = past, gray = future)
- Per-command icons via `dynamic_cast` type detection
- CompoundCommands display a child count with hover tooltips for nested operations
- Current state highlighted via cyan row background (with auto-hiding "Initial State" root node)
- Yellow indicator when `BeginCompound` is actively recording
