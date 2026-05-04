# v2.9.9 — editor-workspaces

**Date:** 2026-05-05
**Commits:** 7 (on `feat/editor-workspaces`)
**Issue:** [#118](https://github.com/Hekbas/Luth/issues/118)
**Series:** AAA editor rework, **closeout** (effort 10 of 10; closes the v2.9.0 → v2.9.9 arc)

---

## Overview

Multi-named workspaces — each pairs an ImGui dock layout with a per-panel
visibility set. Switch / Save As / Rename / Delete user workspaces;
Reset to a built-in baseline; last-active survives restart. Topbar gains
`Window > Workspaces`; `View > Layouts` removed.

Built on the v2.9.7 foundation (`Editor::SaveLayout` / `LoadLayout` /
`GetLayoutNames`, `EditorSettings::activeLayout`, `EditorSettings::panelOpen`).
Three gaps closed:

- `activeLayout` was loaded from `editor_settings.json` but never auto-
  applied at startup — a fresh launch always showed whatever ImGui's
  last state was. Now deferred-applied at end of frame 0.
- `panelOpen` was a single global map. Toggling Console off in workspace
  Foo silently propagated to Bar on next switch. Each workspace now owns
  its own sidecar JSON.
- Workspaces couldn't be renamed or deleted from the UI. Added popups
  mirroring the Save Layout pattern from v2.9.7.

Default workspace ships under `luth/assets/workspaces/Default.{ini,workspace.json}`
following the v2.7.1 EditorStyle precedent (`luth/assets/styles/*.json`).
First-run snapshot at `runtime/layouts/Default.ini` retained as a recovery
target if the engine asset goes missing.

Milestone Release — closes the v2.9.x series. Next series is `jolt-physics`
(v2.10.0) under Mode A.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | NEW `Workspace.{h,cpp}` — `WorkspaceInfo` record + namespace `LoadJson` / `SaveJson` / `IsBuiltinPath`; sidecar shape `{ "panel_open": { <window-id>: bool } }` | [`07c3137`](../../../../commit/07c3137) |
| B | `Editor::SaveLayout` / `LoadLayout` / `GetLayoutNames` replaced by `LoadWorkspace` / `SaveWorkspaceAs` / `RenameWorkspace` / `DeleteWorkspace` / `ResetWorkspaceToBuiltin` / `GetWorkspaces`; built-in path (engine assets) shadows user copy of the same name; `s_NeedActiveWorkspaceLoad` defers the active workspace load to end of first Render so panels and ImGui dock state exist before apply; EditorSettingsWindow General hint updated | [`d19db83`](../../../../commit/d19db83) |
| C | NEW `WorkspaceChangedSignal` on `BusType::MainThread` — name + builtin; `LoadWorkspace` enqueues post-apply; no in-tree subscribers yet | [`afccf1c`](../../../../commit/afccf1c) |
| D | NEW `luth/assets/workspaces/Default.ini` (seeded from existing `runtime/layouts/Default.ini`) + `Default.workspace.json` (11 panel-open keys; `Frame Debugger` + `History` off by default, all others on); no premake change — `luth/assets/styles/*.json` already ships in parallel | [`4b8b558`](../../../../commit/4b8b558) |
| E | Window menu nests Workspaces submenu — radio list with `(builtin)` suffix; `Save Current As…` / `Rename Current…` / `Delete Current…` (Rename + Delete disabled when active is built-in); `Reset to Built-in` replaces `Reset Layout`; Rename input pre-fills with active name; Delete confirmed via modal; View > Layouts block removed entirely; popup state vars renamed `Save/Rename/Delete Workspace` | [`14b868f`](../../../../commit/14b868f) |
| F | NEW `Editor::SaveActiveWorkspaceSidecar()` — snapshots live `panel->m_Open` map, writes sidecar JSON to `runtime/layouts/<active>.workspace.json`. Called on workspace switch (in `LoadWorkspace`, BEFORE the new workspace replaces active state) and from `Shutdown` after `SaveSettings`. Built-in active is a no-op — built-ins are read-only, so panel-visibility tweaks made while a built-in is active persist in-session only | [`a86426f`](../../../../commit/a86426f) |
| Fix | `GetWorkspaces` was emitting `LH_CORE_WARN` on every shadow conflict; the menu calls `GetWorkspaces` per frame, so the warn spammed the log when hovering Window > Workspaces. The shadow case is the first-run Default snapshot (intentional fallback, not user error); `SaveWorkspaceAs` already gates user names against built-in collision. Warn dropped silently | [`503aaf8`](../../../../commit/503aaf8) |
| H | Wrap-up: Version.h, history file, ROADMAP, CLAUDE.md | this commit |

---

## Architectural decisions

### Pair format (`.ini` + `.workspace.json`)

ImGui owns the `.ini` format for dock state and the API
(`SaveIniSettingsToMemory` / `LoadIniSettingsFromMemory`). Embedding
arbitrary JSON inside that file is not supported. Two options for the
visibility map:

1. **Single JSON** with the dock ini blob as a string field.
2. **Pair**: keep `.ini` as ImGui-native; add a sibling `.workspace.json`.

Pair chosen — zero migration of the v2.9.7 first-run `Default.ini`, and
the sidecar evolves freely without touching ImGui's contract. Cost is
two file ops per save / load; both are infrequent.

### Built-in vs user storage layout

Built-ins live under `FileSystem::EngineAssetsPath("workspaces")` —
read-only, ship with the engine. User copies live under cwd-relative
`runtime/layouts/` (the existing first-run snapshot dir; rename to
`workspaces/` would have broken the v2.9.7 `Default.ini` fallback).

Mirrors the v2.7.1 `EditorStyle::LoadFromFile(EngineAssetsPath("styles") /
...)` precedent. Workspaces are personal preferences (per-user, not
per-project), so they don't belong under `<project>/.luth/` alongside
autosaves and thumbnail caches.

### Built-in shadow rule

When a name exists in both dirs, **built-in wins for read**. The user
copy becomes inert in `GetWorkspaces` (silently — see Fix below). The
only path that creates a colliding pair is the first-run Default
snapshot landing in `runtime/layouts/` before any built-in `Default.ini`
exists in engine assets; once the engine ships its built-in, the user
copy becomes a recovery target rather than a primary read source.

`SaveWorkspaceAs` rejects names that collide with a built-in, so user
action can't create a new shadow. Rename / Delete refuse on built-ins
(both `IsBuiltinPath` predicate at the API layer and `MenuItem` enabled
flag in the UI).

### Built-in workspace tweaks don't persist

Toggling a panel while a built-in workspace is active changes
`panel->m_Open` for the rest of the session, but `SaveActiveWorkspaceSidecar`
returns early when active is built-in (would have to write to engine
assets, which are read-only). User must `Save Current As…` to fork into
a customizable copy.

Unity uses the same model — built-in layouts are immutable; modifying
one becomes a "modified" copy.

The `editor_settings.json::panel_open` global map still mirrors the
last-session live state on Shutdown. On next launch:
1. `LoadSettings` populates `s_Settings.panelOpen` from JSON.
2. `ApplyPersistence` syncs into panel `m_Open` — Console = false (from
   last session's tweak in built-in Default).
3. End of frame 0: `LoadWorkspace("Default")` reads built-in sidecar →
   replaces `s_Settings.panelOpen` → `ApplyPersistence` → Console = true.

User-facing result: built-in tweaks last for the rest of the session,
get blown away on restart. One-frame flash of the wrong visibility on
fresh installs is acceptable (Unity has the same flash).

### Auto-apply at end of first Render

`s_NeedActiveWorkspaceLoad` set in `Init` after `LoadSettings`; consumed
in the same first-Render block as the existing `s_NeedDefaultLayoutSave`.
Two reasons not to load earlier:

- Panels must exist (ApplyPersistence reads `s_Panels`).
- ImGui dock state must have built before `LoadIniSettingsFromMemory`
  takes effect — otherwise the load applies but the next ImGui frame
  re-initialises the dock from defaults.

The first-run snapshot still runs on a fresh install. Then the auto-load
fires immediately after, overriding the snapshot's bare default state
with the curated built-in. The runtime snapshot becomes a safety net.

### `LoadWorkspace` saves outgoing sidecar before switching

The plan specced `SaveActiveWorkspaceSidecar` from `Shutdown` only.
Shipped path also calls it from `LoadWorkspace` (before the active state
mutates) so mid-session workspace switches don't lose the outgoing
workspace's panel-visibility tweaks. Cost is one file write per switch,
which is rare.

Without this: toggle Console off in Foo → switch to Bar → switch back
to Foo → Console on (sidecar from disk wins). With it: Console stays
off in Foo across the round-trip.

### `Default only` built-in scope

The plan listed Animation / Lighting / Debug as stretch presets.
Skipped for v2.9.9 — curating useful presets is its own scope (each
needs careful dock arrangement + visibility tuning that benefits
real-use validation). Framework supports drop-in additions: drop a new
`<name>.{ini,workspace.json}` pair into `luth/assets/workspaces/` and
it appears in the menu next launch with no code change.

### Shadow-warn spam fix

`GetWorkspaces` originally emitted `LH_CORE_WARN` on every shadow
conflict. The Window > Workspaces menu calls `GetWorkspaces` once per
frame while hovered, so the warn spammed the log at frame rate. Two
fixes considered:

1. Dedupe per-name (set tracking emitted-once names).
2. Drop the warn — it was speculative future-proofing for a case
   that can't happen via the UI (`SaveWorkspaceAs` already rejects
   built-in collisions).

Option 2 chosen. Comment explains why a future maintainer might be
tempted to re-add it.

---

## Files touched

### New
- `luthien/source/luthien/Workspace.h`
- `luthien/source/luthien/Workspace.cpp`
- `luth/assets/workspaces/Default.ini`
- `luth/assets/workspaces/Default.workspace.json`
- `docs/development/history/v2.x/editor-workspaces.md`

### Edited
- `luthien/source/luthien/Editor.h` — API surface (workspace API + popup state)
- `luthien/source/luthien/Editor.cpp` — API impl, Init wiring, menu, popups, Shutdown
- `luthien/source/luthien/events/EditorSignals.h` — `WorkspaceChangedSignal`
- `luthien/source/luthien/panels/EditorSettingsWindow.cpp` — General hint text
- `luth/source/luth/core/Version.h` — patch bump
- `docs/development/ROADMAP.md` — v2.9.9 row added; planned-epics renumbered; series closeout note
- `CLAUDE.md` — version + In flight + Active series + Next

---

## Series closeout — AAA editor rework (v2.9.0 → v2.9.9)

Ten efforts:

1. **v2.9.0 `editor-foundation`** — Gather→Draw lifecycle; per-panel `LinearAllocator(64*1024)` scratch; 9 panels migrated.
2. **v2.9.1 `editor-signal-bus`** — typed `EditorSignal` events on `EventBus::MainThread`; five signals replace polling.
3. **v2.9.2 `editor-console-errors`** — `Log::AddSink` interface + `ConsolePanel` + per-panel error boundary with stack-trace dump.
4. **v2.9.3 `editor-job-pump`** — `MainThreadPump::Post/Drain` for any-thread → main callbacks; foundation for autosave + thumbnails.
5. **v2.9.4 `editor-autosave`** — periodic side-channel autosave; crash-recovery prompt; first real `MainThreadPump` consumer.
6. **v2.9.5 `editor-thumbnails`** — `ThumbnailCache` + `ThumbnailGenerator` + `ThumbnailPreviewScene`; ProjectPanel grid + Material/Model/Texture inspectors.
7. **v2.9.6 `editor-undo-fix`** — `EditState { changed, committed, itemId }`; per-T pre-edit value stash; release boundaries no longer over-coalesce.
8. **v2.9.7 `editor-panels-polish`** — `widgets/ButtonGroup`; ScenePanel toolbar reorg; HierarchyPanel eye toggle; `Edit > Preferences`; Window menu + `panel_open` map; ProjectPanel `BeginTable` + clipper.
9. **v2.9.8 `editor-inspector-polish`** — Component header ctx menu; `EditorClipboard`; `widgets/InspectorHeader` + `Splitter`; live interactive 3D preview for Material + Model.
10. **v2.9.9 `editor-workspaces`** — multi-named workspaces; per-workspace visibility; built-in Default; `Window > Workspaces` submenu. *(this effort)*

Net: panel system, signal bus, console + log sinks, main-thread pump,
autosave, thumbnails, undo-correctness, panel widget kit, inspector
polish, workspaces. Editor surface now matches AAA baseline (Unity /
Unreal / Godot for the editor primitives that matter).

Next series: **`jolt-physics` (v2.10.0)** under Mode A — the `jolt`
series will MINOR-bump once and tag intermediate efforts with
`jolt.N-<slug>` checkpoint tags rather than per-effort PATCH bumps.
