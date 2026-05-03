# v2.9.4 — editor-autosave

**Date:** 2026-05-03
**Commits:** 4 (on `feat/editor-autosave`)
**Issue:** [#113](https://github.com/Hekbas/Luth/issues/113)
**Series:** AAA editor rework, effort 5 of 8

---

## Overview

First real consumer of `MainThreadPump` (v2.9.3). Adds periodic background scene
autosave with an Unreal-style **side-channel backup** contract: writes go to
`<project>/.luth/autosaves/<scene-stem>-<timestamp>.luth`, never the canonical
scene path. Working file stays exactly at the user's last manual save; the
title-bar `*` persists until the user hits Save. On editor startup, if a scene's
autosave directory contains a file fresher than the canonical scene's mtime,
a modal offers Recover / Discard / Cancel before normal editing resumes.

The serialize-on-main / IO-on-worker split is the design's core constraint: the
EnTT registry walk in `SceneSerializer::SaveToString` must run on the main thread
(V3 hazard — registry mutations would tear). Only the file write hops to a
worker via `IOThread::WriteFile`. Completion lights a 5-second title-bar notice
through `MainThreadPump::Post`, then prunes oldest autosaves down to keep-N.

Tag-only release. The series milestone Release is reserved for `editor-workspaces`
(v2.9.7).

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | `EditorAutoSave.{h,cpp}` + `EditorSettings` extension + `Editor.cpp` wiring (Init/Shutdown/Tick/menu/title-bar) | [`f4f7767`](../../../../commit/f4f7767) |
| B | Crash-recovery prompt (ScanForRecovery + DrawRecoveryModal) | [`f06f3f9`](../../../../commit/f06f3f9) |
| C | Smoke-test fix: lazy timer init + scene auto-load relocation | [`051770e`](../../../../commit/051770e) |
| D | Wrap-up: docs + version bump + history | this commit |

---

## Architectural decisions

### Side-channel only — never overwrite canonical

The autosave file path is `<project>/.luth/autosaves/<stem>-<YYYYMMDD-HHMMSS>.luth`.
`SceneSerializer::Save(scene, canonical_path)` is **never** called from the
autosave path; only `SceneSerializer::SaveToString` (in-memory) plus
`IOThread::WriteFile`. The canonical scene file's mtime stays at the user's
last manual save. This is the Unity / Unreal default behaviour and the
contract the user validated up front: autosave protects against crashes
without surprising the user with file changes they didn't authorise.

The dirty flag is correspondingly **never cleared** by autosave. `Editor::IsDirty`
is read-only from the autosave subsystem; only manual `SaveScene` clears the
asterisk.

### Serialize-on-main, IO-on-worker

`SceneSerializer::SaveToString` walks the EnTT registry: `view`, `get<T>`, etc.
Concurrent main-thread mutations would tear. The autosave's main-thread `Tick()`
synchronously calls `SaveToString` to capture a consistent JSON snapshot, then
hands the resulting `std::vector<u8>` to `IOThread::WriteFile` (fire-and-forget
async). Completion goes through `MainThreadPump::Post` for the on-main UI flash
and the prune step.

A V3-anchored comment block on `DispatchWrite` documents the invariant explicitly
so the next maintainer doesn't try to "optimise" the serialization onto a worker.

### `MPMCQueue` rejected for the autosave's transport

The pump itself uses `std::queue` + `std::mutex` (v2.9.3 decision). Autosave
inherits — no new transport primitive.

### Toast UI deferred — title-bar suffix instead

Autosave success surfaces as ` — Autosaved HH:MM` appended to the window title
for ~5 seconds, fading via a `Time::GetTime()`-gated check in `UpdateWindowTitle`.
A real toast widget (corner overlay, fade-out, multi-line) is its own future
effort. The title-bar approach reuses an existing main-thread sink, ships in
under 20 lines, and matches Unreal's bottom-corner ticker in spirit.

### Crash-recovery prompt — `<stem>-*.luth` mtime vs canonical

`ScanForRecovery(scenePath)` lists `<project>/.luth/autosaves/`, filters by
`<stem>-*.luth`, picks the freshest by `last_write_time`, and compares against
the canonical scene's mtime. Strictly greater → recovery pending. The modal
shows the filename, timestamp, and full autosave path, with three buttons:

- **Recover** — `SceneSerializer::Load(*scene, autosave_file)`, then `Editor::MarkDirty()` because the loaded content differs from the canonical file on disk; the user has to hit Save to persist.
- **Discard** — `fs::remove(autosave_file)`, canonical scene stays loaded.
- **Cancel** — close the modal, leave the autosave file in place. Next autosave cycle won't touch it (different timestamp), so the user can change their mind via the file system.

### Hook placement — `OpenScene`, not `SetActiveScene`

The original sub-task B hook was `Editor::SetActiveScene`'s startup auto-load
branch. Smoke-test surfaced that **the auto-load was already broken in
mainline**: `SetActiveScene` runs from `App::App()` at line 98, *before*
`LoadProject` populates `AssetDatabase`. The `AssetDatabase::Exists(uuid)` check
always returned false → inner branch skipped → `lastSceneUUID.clear()` wiped the
in-memory state before the database was live. By the time `LoadProject`
finished, `lastSceneUUID` was empty. No scene auto-loaded; the recovery hook
inside the dead branch never fired.

Sub-task C moves the auto-load body to `Editor::OnProjectChanged`, which runs
*after* `LoadProject` populates `AssetDatabase`. The recovery scan moves into
`Editor::OpenScene`'s success path so it covers both the auto-load flow and
manual `File > Open`. A user who manually opens a scene with a fresher autosave
gets the recovery prompt — exactly the behaviour they want when re-opening a
scene they crashed on.

`SetActiveScene`'s body collapses to panel-context updates; a comment points at
the new home for the auto-load logic.

### Lazy timer init in `Tick`

`EditorAutoSave::Init` runs at the end of `Editor::Init`, which happens inside
`App::App()` at line 97. `Time::Update()` first runs in `App::Run` at line 176
— after `App::App()` returns. So at `Init`-time, `Time::s_StartTime` is
default-constructed (epoch-zero), and `Time::GetTime()` returns "seconds since
implementation-defined steady_clock zero" (on Windows: system uptime). Capturing
that into `s_LastSaveTime` made the per-frame gate `Time::GetTime() - s_LastSaveTime`
always negative-and-huge, so the interval check forever short-circuited. Autosave
never fired.

Sub-task C moves the capture into the first `Tick()` call, gated by an
`s_TickArmed` bool reset by `Init()`. By the first Tick, `Time::Update()` has
run at least once and the steady-clock baseline is correct.

### V1-V6 hazard mapping

| Hazard | Status | Note |
|---|---|---|
| V1 lock contention | N/A | All autosave state is main-thread-only |
| V2 main-thread starvation | N/A | Edge frequency (≥60s default); SaveToString is 10-50 ms |
| V3 cross-thread ECS | **mitigated** | SaveToString on main, only IOThread hops |
| V4 lost wakeup | N/A | No sleep/wait protocol |
| V5 sub-job context thrash | N/A | No nested dispatch |
| V6 GPU↔allocator deadlock | N/A | `Memory::Category::Editor`, not `FrameTagged` |

### `AssetManager::s_UploadQueue` deliberately not migrated

Considered — but its drain is a typed pipeline stage (typed dispatch on
`AssetType`, mid-iteration `LoadAsync` recursion, GPU-fence ordering vs
`UploadContext::DrainPendingBinds`). Erasing into opaque callbacks would
either spread typed branching across post sites or hide it behind unreadable
lambdas. Two queues, two purposes — `MainThreadPump` for opaque worker→main
hops, `AssetManager::s_UploadQueue` for asset finalisation.

### No exit-flush on shutdown

`EditorAutoSave::Shutdown` doesn't autosave-on-close. Comment-anchored
`invariant: shutdown does not autosave`. Reasoning: a final autosave during
shutdown would race engine teardown (`IOThread::Shutdown`, `MainThreadPump`
state, etc.) and the user keeps full control via manual Save anyway.

### No Settings panel UI

`autoSaveEnabled`, `autoSaveIntervalSec`, `autoSaveKeepN` ship in `EditorSettings`
with JSON load/save plumbing — but no in-editor UI to toggle them. A Settings
panel is its own future effort. Manual JSON edit is fine for v2.9.4 since
the defaults work.

---

## Files & locations

### New
- `luthien/source/luthien/EditorAutoSave.h` — public statics (`Init/Shutdown/Tick/ForceNow/GetLastNotice/IsNoticeActive/ScanForRecovery/DrawRecoveryModal`).
- `luthien/source/luthien/EditorAutoSave.cpp` — file-static state, `DispatchWrite`, prune, recovery scan + modal.

### Modified — engine
- `luth/source/luth/core/Version.h` — bumped to `v2.9.4`.

### Modified — editor (luthien)
- `luthien/source/luthien/EditorSettings.h` — three new fields (`autoSaveEnabled`, `autoSaveIntervalSec`, `autoSaveKeepN`).
- `luthien/source/luthien/EditorSettings.cpp` — Load/Save plumbing for the three fields.
- `luthien/source/luthien/Editor.h` — `GetScenePath()` accessor for `EditorAutoSave`.
- `luthien/source/luthien/Editor.cpp` — `EditorAutoSave::Init/Shutdown/Tick` wiring; `File > Autosave Now` menu entry; title-bar suffix in `UpdateWindowTitle`; `ScanForRecovery` moved to `OpenScene` success path; auto-load body relocated from `SetActiveScene` to `OnProjectChanged`; `DrawRecoveryModal` called from `Render`'s modal block.

### Modified — docs
- `docs/development/ROADMAP.md` — v2.9.4 row in completed table.
- `CLAUDE.md` — Current Progress block updated (untracked).
- `docs/development/history/v2.x/editor-autosave.md` — this file.

---

## Build Verification

4 atomic commits on `feat/editor-autosave`; every commit builds Debug x64 clean
(pre-existing C4996 / C4244 baseline only). Smoke test confirms:

- **Force-now happy path.** `File > Autosave Now` writes `<project>/.luth/autosaves/<stem>-<TS>.luth`. Title-bar gets `Autosaved HH:MM` for ~5 s. Dirty `*` persists.
- **Interval gate.** With `autoSaveIntervalSec=5`, periodic writes fire while dirty. Lazy timer init confirmed working — autosave triggers automatically without manual force.
- **Pruning.** `autoSaveKeepN=3` + 5 force-now calls leave exactly 3 files in the directory.
- **Play-mode gate.** Enter Play, no writes; Stop, full interval before next save (countdown reset).
- **Untitled / foreign-path gate.** New Scene + force-now → no file, single WARN. Foreign-root scene → no file, single WARN.
- **Crash recovery.** Edit, force-now, kill via Task Manager, restart. Editor auto-loads scene; `OpenScene` triggers `ScanForRecovery`; modal shows timestamp + path. Recover loads autosave + marks dirty. Discard deletes autosave. Cancel leaves file.
- **Auto-load fix dividend.** Last-opened scene now actually re-opens on editor startup — long-broken behaviour restored as a side-effect of fixing the recovery hook.
- **Editor shutdown clean.** No leak, no race, no use-after-free.

Closes #113.
