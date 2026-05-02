# v2.9.1 — editor-signal-bus

**Date:** 2026-05-02
**Commits:** 7 (on `feat/editor-signal-bus`)
**Issue:** [#110](https://github.com/Hekbas/Luth/issues/110)
**Series:** AAA editor rework, effort 2 of 8 — bundled with EventBus hardening

---

## Overview

Layers typed `EditorSignal` events on the existing `EventBus::BusType::MainThread` so editor panels can react to selection / hierarchy / asset / project / play-state changes via subscriptions instead of polling. Replaces the `Scene::GetHierarchyVersion` polling loop in `Editor::Render` with a `HierarchyChangedSignal` subscription that bumps the dirty flag reactively.

Bundles a hardening pass on `EventBus` itself: a pre-effort audit returned **YELLOW** with five real flaws that compounded at the new usage volume (5 signal types × ~10 panel subscribers × ~10 events/sec). Without the bundle, layering new traffic on top of the existing implementation would have silently dropped events on handler exception, leaked dangling pointers if panels were ever destroyed, and shipped untracked allocations through `MemoryTracker`.

Tag-only release per the "would a portfolio reviewer care?" rule — this is foundational plumbing for the rest of the editor-aaa series, not a user-visible feature.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | EventBus exception-safe dispatch + thread assert + reentrancy comment | [`5d103c4`](../../../../commit/5d103c4) |
| B | EventBus `SubscriptionHandle` + `Unsubscribe` for panel-lifetime safety | [`a4d2746`](../../../../commit/a4d2746) |
| C | EventBus tracked allocations via custom `EventDeleter` + `MemoryTracker` | [`4e55d90`](../../../../commit/4e55d90) |
| D | New `EditorSignals.h` — 5 typed signals on the bus | [`669ee35`](../../../../commit/669ee35) |
| E | Wire publishers: `EditorSelection`, `EntityCommands`, `PlayModeController`, `Editor::OnProjectChanged`, `AssetDatabase` callback forwarder | [`2ca8817`](../../../../commit/2ca8817) |
| F | Replace `Scene::GetHierarchyVersion` polling with `HierarchyChangedSignal` subscription | [`8aa2802`](../../../../commit/8aa2802) |
| G | Wrap-up: docs + version bump + history | this commit |

---

## EventBus audit findings

The audit ran before any code changes. Verdict: **YELLOW** — functionally correct for the prior light usage but with five flaws that v2.9.1's volume would expose:

1. **No exception safety in dispatch.** A thrown handler aborted the loop, dropping subsequent handlers and remaining queued events silently. Sub-task A wraps `handler(event)` in a `std::exception` + catch-all guard that logs and continues; `m_Handled` propagation is preserved across a thrown handler (treated as non-consuming).

2. **No `Unsubscribe`.** Subscriptions were permanent. A panel that registered a lambda capturing `this` and was later destroyed would leave a dangling pointer; the next dispatch would call into freed memory. Sub-task B adds `SubscriptionHandle` (typeID + monotonic id) returned from `Subscribe`, with a paired `Unsubscribe(handle)`. Existing process-lifetime callers in `App.cpp` (window-resize, window-close, file-drop) are unchanged — they discard the return value.

3. **No thread assertion on `ProcessEvents`.** A worker fiber accidentally calling `ProcessEvents(BusType::MainThread)` would race the main thread on `m_Subscribers`/`m_EventQueue`. Sub-task A captures the dispatch thread on first call (per bus, debug-only) and asserts subsequent calls match.

4. **Untracked allocations.** `std::make_unique<T>(...)` allocated through `::operator new` — visible to Tracy's global hook but invisible to the engine's `MemoryTracker`. ProfilerPanel's per-category bars couldn't budget event traffic. Sub-task C introduces a polymorphic `EventDeleter` that captures `(category, sizeof(T))` at Enqueue time and records the matching free in its `operator()` so the unique_ptr's destruction path is fully tracked. Allocation moved outside the queue lock — payload construction can be non-trivial and we don't want it serialised.

5. **Reentrancy semantics undocumented.** Events enqueued during `ProcessEvents` go into `m_EventQueue`, not the local `processingQueue` — they fire on the next drain. This was correct but unstated; sub-task A documents the contract above `ProcessEvents`. Synchronous re-dispatch was rejected: handler chains risk unbounded recursion and same-frame follow-on work should write to panel state instead.

The deferred sixth audit item — subscriber priority — is not needed at the v2.9.1 volume (~10 subscribers per type max). It can layer later if a future epic requires strict ordering.

---

## EditorSignal taxonomy

Five signals, all UUID-based (never raw `entt::entity`, per the v2.7.0 command precedent — handles stay valid across destroy-undo cycles):

- **`SelectionChangedSignal`** — version + entity UUID list + selected resource UUID. Published by every `EditorSelection` mutator after its version bump.
- **`HierarchyChangedSignal`** — Op enum (Created / Destroyed / Reparented / Reordered / Renamed) + entity UUID + parent UUID (valid only for Reparented). Published by every `Entity*Command::Execute / Undo / Redo`. `GizmoTransform` and `EntityActive` deliberately skip — those aren't structure changes.
- **`AssetChangedSignal`** — Op enum (Imported / Modified / Deleted) + asset UUID. The current `AssetDatabase` callback API only exposes the dirty-UUID list, not per-asset op, so v2.9.1 publishes `Modified` for every change; subscribers that care about distinction query `AssetDatabase::Exists(uuid)` themselves. A future API refinement could expose op directly.
- **`ProjectChangedSignal`** — path + project name (string, not `std::filesystem::path`, to keep the header light). Path empty on unload.
- **`PlayStateChangedSignal`** — from + to. Published by every `PlayModeController` transition.

`GetCategoryFlags()` returns `EventCategory::None` for all editor signals — they're dispatched by exact type, not by category bitmask. `ProjectChangedSignal::GetProjectName()` is named non-obviously to avoid collision with `Event::GetName()` (the type-identification virtual).

---

## Polling-removal sites

The pre-rework `Editor::Render` poll (`luthien/source/luthien/Editor.cpp` L404-411) and its `s_LastHierarchyVersion` stamp infrastructure are gone:

- `Editor::Render` — polling block deleted.
- `Editor::Init` — subscribes to `HierarchyChangedSignal`; handler is `[](Event&) { Editor::MarkDirty(); }`.
- `Editor::SetActiveScene` / `NewScene` / `OpenScene` — version-stamp lines removed.
- `Editor::ResetDirtyState` — version-stamp line removed; method now just clears `s_IsDirty`.
- `Editor.h` — `s_LastHierarchyVersion` member declaration removed.

Scene LOAD does not fire `HierarchyChangedSignal` (deserialization bypasses commands), so the dirty flag stays clean across open/close — the prior version-stamp reset on load was working around the same polling path that's now gone.

---

## Architectural decisions

### EditorSelection split into header + cpp

Pre-v2.9.1, `EditorSelection` was a header-only static singleton with all mutators inline. Layering signal publishing required including `EditorSignals.h` (which transitively pulls `EventBus.h` and `Components.h` for the UUID conversion). Inlining that include chain into every translation unit that touches selection would have inflated build times noticeably.

The split is mechanical: header keeps the static state declarations + cheap accessors (`GetSelectedEntity`/`GetSelectedEntities`/`GetVersion`/etc.), `EditorSelection.cpp` defines the mutators and an anonymous-namespace `PublishSelectionChanged` helper that walks the selection set, converts to UUIDs, and enqueues the signal. The header still exposes inline `IsSelected` (just an STL find) since it has no heavy dependency.

### Per-command publishing vs. central tap

Considered a single `CommandHistory::Execute`-side tap that fired one generic `HierarchyChangedSignal` per command, regardless of type. Rejected: lost the Op granularity that future Subscribers (FrameDebugger overlays, panel-side caches) will want. The chosen approach — one publish line per `Execute / Undo / Redo` body — is mechanical (per-command line count: 1) and preserves type information.

`Op::Renamed` fires on both `EntityRenameCommand::Execute` and `Undo` — both are user-induced renames from the editor's perspective.

### Why no `IEditorHooks` extension

The architecture plan called for `IEditorHooks::OnHierarchyChanged` / `OnAssetChanged` / `OnPlayStateChanged` as engine→editor signal forwarders. v2.9.1 doesn't need them: every publisher (EditorSelection, EntityCommands, PlayModeController) lives editor-side on main, and `AssetDatabase::AddChangeCallback` already provides a callback API the editor uses to forward into `EventBus`. A future engine-side publisher (e.g., physics events from jolt) would justify the extension; v2.9.1 doesn't.

### Tracked allocation pattern

`LH_NEW(Memory::Category::General, T, ...)` doesn't compose with `unique_ptr` because the category has to be remembered for the deleter. The audit's "use LH_NEW" recommendation translated into a polymorphic `EventDeleter` struct that captures `(category, sizeof(T))` at Enqueue and records the free in its `operator()`. Allocation uses raw `new T(...)` followed by an explicit `MemoryTracker::RecordAlloc` — this is the same shape `LH_NEW` expands to, just inlined where the deleter capture is set up. Tracy's global `new` hook still picks up the raw allocation; `MemoryTracker` adds the category bucket on top per `arch/memory.md` policy.

---

## Files touched

### New
- `luthien/source/luthien/events/EditorSignals.h` — 5 typed signal classes.
- `luthien/source/luthien/EditorSelection.cpp` — mutator definitions + `PublishSelectionChanged` helper.

### Modified — engine
- `luth/source/luth/events/EventBus.h` — exception-safe dispatch, `SubscriptionHandle`/`Unsubscribe`, `EventDeleter` + tracked allocations, thread assert, reentrancy doc.
- `luth/source/luth/core/Version.h` — bumped to v2.9.1.

### Modified — editor (luthien)
- `luthien/source/luthien/EditorSelection.h` — split: header keeps state + cheap accessors, mutators move to .cpp.
- `luthien/source/luthien/commands/EntityCommands.cpp` — `PublishHierarchy` helper; 12 publish call-sites across Create/Destroy/Rename/Reparent/Reorder/Duplicate Execute+Undo+Redo.
- `luthien/source/luthien/PlayModeController.cpp` — `PublishPlayState` helper; EnterPlay/Pause/Resume/Stop call it.
- `luthien/source/luthien/Editor.cpp` — `AssetDatabase::AddChangeCallback` forwarder + `ProjectChangedSignal` publish in `OnProjectChanged` + `HierarchyChangedSignal` subscription replaces polling block; `s_LastHierarchyVersion` purged from 4 sites.
- `luthien/source/luthien/Editor.h` — `s_LastHierarchyVersion` member declaration removed.

### Modified — docs
- `docs/development/history/v2.x/editor-signal-bus.md` (this file).

---

## Build Verification

7 atomic commits on `feat/editor-signal-bus`; every commit builds Debug x64 clean with no new warnings beyond the existing pre-rework set. Editor smoke test confirms identical behaviour to v2.9.0:

- Selecting / multi-selecting / clearing entities triggers `SelectionChangedSignal` once per click.
- Renaming, destroying, reparenting entities still updates the dirty flag (now via signal handler) and bumps the title-bar asterisk.
- Play / Pause / Resume / Stop fire `PlayStateChangedSignal` transitions.
- File-watch hot-reload triggers `AssetChangedSignal::Modified` within ~1s.
- Scene Open / New leaves dirty flag clean.

No new Vulkan validation errors. Closes #110.
