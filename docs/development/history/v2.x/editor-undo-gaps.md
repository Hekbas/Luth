# v2.7.3 — editor-undo-gaps

**Date:** 2026-04-23
**Commits:** 6 (on `epic/editor-undo-gaps`)
**Issue:** [#88](https://github.com/Hekbas/Luth/issues/88)

---

## Overview

Wrap the 14 `Editor::MarkDirty()` callsites in `InspectorPanel.cpp` so the
edits go through `CommandHistory` and survive Ctrl+Z. Most are vector-element
edits inside `AnimationController::Layers` — they bypassed undo because
`ComponentPropertyCommand<C, T>` only addresses single member-pointers, not
`vec[i].field`. Adds three new vector command templates, an `EntityActiveCommand`,
and an `EXEC_COMPONENT_PROP` macro that collapses the existing 6-line
`make_unique<ComponentPropertyCommand<...>>(...)` boilerplate.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | `commands/VectorCommands.h` (`VectorElementProperty`/`VectorInsert`/`VectorErase`) + `EntityActiveCommand` | [`2fd6022`](../../../../commit/2fd6022) |
| B | Wrap per-field layer edits — `ClipIndex`/`Weight`/`Speed`/`Loop`/per-bone `BoneMask[i]` (5 callsites) | [`dd82e4d`](../../../../commit/dd82e4d) |
| C | Wrap vector mutations — `+ Add Layer`/`Remove Layer`/`BoneMask All`/`None`/`Clear` (5 callsites); drop spurious `MarkDirty()` on `EditorSettings::showBoneDebug` | [`a399ab0`](../../../../commit/a399ab0) |
| D | Wrap entity active toggle (1) + `BoneAttachment` Target/Bone combos via compound (2 compound = 5 child commands); audit other panels | [`3099aa1`](../../../../commit/3099aa1) |
| E | `EXEC_COMPONENT_PROP` macro + apply to 8 representative existing callsites | [`1e749f5`](../../../../commit/1e749f5) |
| F | Version bump + docs | this commit |

---

## Key Changes

- **`VectorElementPropertyCommand<C, Elem, T>`** — addresses `(comp.*VecMember)[idx].*FieldMember`. Two `T C::*`-style member pointers + a `size_t` index. Re-resolves the entity by UUID on each Apply (same pattern as `ComponentPropertyCommand`); merge-coalescing keys on UUID + vector ptr + index + field ptr so slider drags collapse into a single undo entry.
- **`std::vector<bool>` workaround** — per-bone-checkbox can't address into `BoneMask` via `T Elem::*` because `vector<bool>::operator[]` returns a proxy. Fix: snapshot the whole `BoneMask` vector. For typical 50-200 bone skeletons that's ~200 bytes per click, trivial.
- **`VectorInsertCommand` / `VectorEraseCommand`** — symmetric pair. Insert stores the element by value, Execute inserts at `[index]`, Undo erases at `[index]`. Erase snapshots `vec[index]` at construction, Execute erases, Undo re-inserts the snapshot.
- **`EntityActiveCommand`** — mirrors `EntityRenameCommand`: stores Scene*, UUID, old/new bool; Apply does `Scene::FindEntityByUUID(uuid).SetActive(value)`. The wrapper-only `Entity::isActive` storage design is a separate concern — left for a future `Entity` cleanup epic.
- **`BoneAttachment` compound commands** — Target combo writes 3 fields (`TargetEntity`/`BoneIndex`/`BoneName`); Bone combo writes 2 (`BoneName`/`BoneIndex`). Both wrapped in `BeginCompound`/`EndCompound` so undo restores all fields in one Ctrl+Z press, not piecemeal.
- **Spurious `showBoneDebug` `MarkDirty` removed** — `EditorSettings::showBoneDebug` is an editor preference saved to `editor_settings.json`, not scene state. Marking the scene dirty for it was wrong; no command added (editor toggles aren't on the scene undo stack).
- **`EXEC_COMPONENT_PROP(NAME, SCENE, ENTITY, COMP, MEMBER, OLD, NEW)`** macro derives `T` via `std::decay_t<decltype(OLD)>`, drops the explicit template parameter list, and collapses the 6-line boilerplate to 1. Applied to 8 representative existing callsites (Transform Position/Rotation/Scale, Camera FOV/Near, AnimationController CurrentClipIndex/ApplyRootMotion/DefaultTransitionDuration). Remaining ~30 callsites stay as-is for incremental migration in later epics.
- **Audit results** — `CommandHistory.cpp:36/48/60/87` `MarkDirty` calls are correct (the system itself marks the scene dirty after every Execute/Undo/Redo/EndCompound). `ScenePanel.cpp:174` (Browse HDR) documented out-of-scope: async IBL reload + one-shot file dialog need design before wrapping.

---

## Build Verification

- 6 atomic commits on `epic/editor-undo-gaps`; every commit builds Debug x64 clean.
- Runtime smoke (user-tested): drag every animation-layer slider (Speed/Weight), toggle Loop, change Clip combo, click All/None/Clear in BoneMask, click + Add Layer / Remove Layer, toggle Entity active checkbox, change BoneAttachment Target then Bone — every action undoes and redoes correctly.

---

## Next

`editor-component-registry` — replace the hand-written switch in `InspectorPanel::DrawEntityComponents` with a `ComponentDrawerRegistry` (type-erased per-component drawers, auto-populated Add Component menu). Highest structural ROI of the editor-review series; expected to shrink `InspectorPanel.cpp` by ~40%. Or pivot to `play-mode` (v2.8.0) feature work.
