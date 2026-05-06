# v2.9.6 — editor-undo-fix

**Date:** 2026-05-04
**Commits:** 4 (on `fix/editor-undo-fix`)
**Issue:** [#115](https://github.com/Hekbas/Luth/issues/115)
**Series:** AAA editor rework, effort 7 of 9 (reframed)

---

## Overview

Slider-driven inspector edits over-coalesced across release boundaries. Edit
PointLight Intensity 1→2, release; 2→3, release; 3→4, release — and the undo
stack collapsed to a single entry holding (oldValue=1, newValue=4). The user
intended three discrete edits.

The fix matches Unity / Unreal: each `IsItemDeactivatedAfterEdit` boundary =
one undo entry, with no cross-release merging. Pre-edit value snapshotted on
`IsItemActivated` and consumed on commit.

This effort was originally framed as "transactional undo" with
`BeginTransaction` / `CommitTransaction` primitives. Audit during planning
surfaced that gizmo drag was already correct (via `m_WasUsing` transition +
`CanMerge=false`); only the merge override on `ComponentPropertyCommand` and
`VectorElementPropertyCommand` was wrong. No transaction primitive needed —
`CompoundCommand` already covers grouped commits where genuinely required.

Tag-only release. The series milestone Release is reserved for
`editor-workspaces` (v2.9.9, was v2.9.7 — series reframed mid-flight to also
include `editor-panels-polish` and `editor-inspector-polish`).

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | `EditState` struct + per-T `PreEditStore<T>()` Meyers-singleton map; every `UI::Property*` + `PropertyAsset` returns `EditState`; back-compat `operator bool` keeps existing call sites compiling | [`9dcb6f4`](../../../../commit/9dcb6f4) |
| B | Drop `CanMerge`/`MergeWith` on `ComponentPropertyCommand` + `VectorElementPropertyCommand`; migrate 8 component drawers to push on `state.committed`; direct `ImGui::SliderFloat` sites in `AnimationControllerDrawer` use the inline activate/deactivate pattern | [`3647d4f`](../../../../commit/3647d4f) |
| — | Fix: `Property(bool&)` commits synchronously on click — `IsItemDeactivatedAfterEdit` is unreliable for click-toggle widgets where activation + edit + deactivation collapse into one frame | [`12c36da`](../../../../commit/12c36da) |
| C | Wrap-up: docs + version bump + history | this commit |

---

## Architectural decisions

### Strategy A (push on commit) over Strategy B (release-aware merge cycle)

Two viable approaches:
- **A** — push exactly once per `IsItemDeactivatedAfterEdit` boundary, with a
  pre-edit value captured on `IsItemActivated`. Drop `CanMerge` for property
  commands.
- **B** — keep per-frame pushes, tag commands with an "edit cycle ID" that
  increments on `IsAnyItemActive` transition, only merge within a cycle.

A wins. Strategy B keeps allocating N commands per drag and merging N-1 away
— A allocates one command total. A also drops two `CanMerge` / `MergeWith`
overrides without replacement, net code reduction. The merge facility in
`CommandHistory::Execute` (CommandHistory.cpp:29-32) survives untouched and
remains generic for any future command that genuinely benefits.

The chosen pattern mirrors `GizmoController.cpp:60-92`'s `m_WasUsing`
transition — that worked correctly already and uses the same conceptual
shape (snapshot on first frame of gesture, push once on the last frame).
This fix generalizes that to ImGui's per-item lifecycle.

### Per-T pre-edit storage via Meyers singleton

The new gap is that ImGui tracks the active-item lifecycle but doesn't
persist the pre-edit *value* across the activation→commit window. Filled
with one templated `static std::unordered_map<ImGuiID, T>` per T, accessed
via `UI::PreEditStore<T>()`. Main-thread only by ImGui contract — no
`std::mutex`, no `thread_local`, no allocator beyond what `unordered_map`
already does. Bounded by the count of simultaneously-active items (≤ 1 in
practice, since ImGui only allows one active item at a time). A sanity
guard `if (size > 32) clear()` handles aborted-edit leakage (window closed
mid-drag).

### `EditState::operator bool` for migration safety

Sub-task A changed every `UI::Property*` return type from `bool` to
`EditState`. Without back-compat, every call site would have to migrate in
the same commit — high blast radius for a refactor that should be
behavior-preserving. `operator bool() const noexcept { return changed; }`
preserves `if (UI::Property(...))` semantics so sub-task A ships
zero-behavior-change. The drawer migration in sub-task B is then purely
adopting the new API where it adds value.

### Discrete widgets commit synchronously

`Property(bool&)` (Checkbox), `PropertyCombo`, and `PropertyAsset` (drag-drop +
right-click clear) are one-frame discrete gestures. ImGui's
`IsItemDeactivatedAfterEdit` is unreliable for click-toggle widgets where
activation + edit + deactivation collapse into a single frame — the deferred
event may not fire at all (or fires inconsistently across ImGui versions).
For these three: `if (changed) { SaveItemPreEdit; committed = true; }`
synchronously. Sliders, drags, color pickers, and text inputs use the
deferred deactivated-after-edit path because they have a real continuous-edit
phase that the synchronous shortcut would mis-coalesce.

The Checkbox case wasn't caught by the original audit — surfaced during the
sub-task B smoke test (Cast Shadows toggle didn't record). The `12c36da`
fix-up applies the same discrete-commit pattern uniformly.

### Multi-axis Vec2/3/4 — composite snapshot

`DrawVecControl` calls 2-4 `ImGui::DragFloat` per row. Each axis has its own
ImGuiID and own activation/deactivation lifecycle. The OLD value captured
for the undo command should be the WHOLE composite (all axes), not just the
axis that the user touched — undoing a Position.X edit shouldn't reset Y/Z
back to whatever they were when X was first activated.

Implementation: snapshot `T preComposite = value` at function entry. On the
first axis to fire `IsItemActivated` this frame (guarded by a local
`savedThisFrame` bool), save the whole composite under the row's parent ID
(`ImGui::GetID(label)`). On any axis's `IsItemDeactivatedAfterEdit`, set
`committed = true`. The reset button (per-axis, no
`IsItemDeactivatedAfterEdit` emission) treats the click branch as a
synchronous commit using the function-entry snapshot.

The `preComposite` capture is safe across frames because save only fires on
the activation frame (first click), where the composite hasn't been mutated
yet by drag motion (drag deltas accumulate on subsequent frames).

---

## Files modified

| File | Change |
|---|---|
| `luthien/source/luthien/widgets/Properties.h` | Add `EditState` + per-T `PreEditStore<T>` + Save/TryGet/ConsumeItemPreEdit helpers; every `Property*` returns `EditState` |
| `luthien/source/luthien/widgets/Properties.cpp` | Each overload's body wires activation save + commit detection; `DrawVecControl` templated on T for composite snapshot; `Property(bool&)` / `PropertyCombo` synchronous commit |
| `luthien/source/luthien/widgets/AssetSlot.h` | `PropertyAsset` returns `EditState` |
| `luthien/source/luthien/widgets/AssetSlot.cpp` | Discrete commit on drop / clear |
| `luthien/source/luthien/commands/ComponentPropertyCommand.h` | Drop `CanMerge` + `MergeWith` overrides |
| `luthien/source/luthien/commands/VectorCommands.h` | Drop `CanMerge` + `MergeWith` on `VectorElementPropertyCommand` |
| `inspectors/component_drawers/TransformDrawer.cpp` | Migrate 3 sites; keep `IsDirty=true` on `state.changed` so visuals follow the drag |
| `inspectors/component_drawers/CameraDrawer.cpp` | Migrate 8 sites (Combo + 7 floats); same `IsDirty` rule |
| `inspectors/component_drawers/PointLightDrawer.cpp` | Migrate 3 sites |
| `inspectors/component_drawers/DirectionalLightDrawer.cpp` | Migrate 5 push sites; un-pushed `Property` calls work unchanged via back-compat |
| `inspectors/component_drawers/MeshRendererDrawer.cpp` | Migrate Model + Material `PropertyAsset`; Mesh Index `Property<int>` with apply-on-changed / push-on-committed |
| `inspectors/component_drawers/AnimationDrawer.cpp` | Migrate Clip / Speed / Timeline; Play/Pause/Stop/Loop button push sites unchanged |
| `inspectors/component_drawers/AnimationControllerDrawer.cpp` | Migrate `PropertyAsset` sites; direct `ImGui::SliderFloat` sites (Transition / Layer Weight / Layer Speed) use inline IsItemActivated + IsItemDeactivatedAfterEdit pattern |
| `inspectors/component_drawers/BoneAttachmentDrawer.cpp` | Migrate Local Offset / Local Rotation Vec3 sites; compound Combos (Target / Bone) unchanged |
| `luth/source/luth/core/Version.h` | `VERSION_PATCH` 5 → 6 |

Net delta: ~370 LOC added, ~190 removed across 14 files. `MaterialEditor`
debounced `MaterialSnapshotCommand` deliberately untouched — different
mechanism, already correct (no `CanMerge` override, debounce window produces
distinct undo entries).

---

## Out of scope (deliberate)

- **`MaterialEditor`** — debounced `MaterialSnapshotCommand` produces
  correct distinct entries. Aligning its commit semantics with the rest of
  the inspectors (commit on slider release vs. debounce timer) is a
  candidate for `editor-inspector-polish` (v2.9.8) when the live-preview
  work touches the same surface.
- **No new transaction primitive.** `CompoundCommand` already groups discrete
  commits (`BoneAttachmentDrawer`'s Target / Bone setters use it). The
  original `BeginTransaction` / `CommitTransaction` framing was YAGNI.
- **`Property*` calls in `RenderPanel` / `ScenePanel` / `TextureEditor` /
  `ModelViewer`** — these mutate editor settings or import options without
  pushing commands. No undo expected; no migration needed.

---

## Verification

Manual smoke test in editor (no automated harness for ImGui interactions):

| # | Case | Result |
|---|---|---|
| 1 | Float drag — PointLight Intensity, 3 separate releases → 3 entries | ✅ |
| 2 | Vec3 drag — Position.X drag, Position.Y drag, X reset → 3 entries | ✅ |
| 3 | Color picker — PointLight Color, drag hue → 1 entry | ✅ |
| 4 | Combo — Camera Projection toggle twice → 2 entries | ✅ |
| 5 | Bool — Cast Shadows on/off/on → 3 entries (after `12c36da` fix-up) | ✅ |
| 6 | PropertyAsset — drag Material, then right-click Clear → 2 entries | ✅ |
| 7 | Direct `ImGui::SliderFloat` — Transition slider 2 separate drags → 2 entries | ✅ |
| 8 | Gizmo regression — translate gizmo drag → 1 entry, unchanged | ✅ |
| 9 | MaterialEditor regression — debounced snapshot still fires | ✅ |
| 10 | Compound — Bone Attachment Target combo → 1 compound w/ 3 children | ✅ |
| 11 | Undo/redo walk — Ctrl+Z/Y bounce through 5 entries | ✅ |
| 12 | Play mode — slider edits discarded, no undo entries on Stop | ✅ |
