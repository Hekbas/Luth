# v2.0.0 — arch-target-split

**Date:** 2026-04-18
**Commits:** 5
**Issue:** [#78](https://github.com/Hekbas/Luth/issues/78)

---

## Overview

Phase 5 of the architecture refactor — the final `arch-*` epic. Extracted ~12 071 LOC of editor code from `Luth.lib` into a new `Luthien.lib` static library, renamed the `luthien/` exe folder to `runtime/`, and broke the engine→editor include dependency via an `IEditorHooks` interface. After this epic, `Luth.lib` has zero `luthien/...` includes and the one-way-dependency invariant is enforced by a `git grep` check.

Major version bump to **v2.0.0** per the ROADMAP versioning rule (fundamental architecture change).

See the multi-epic plan: [`docs/development/ARCH-REFACTOR-PLAN.md`](../../ARCH-REFACTOR-PLAN.md).

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | Rename `luthien/` exe folder to `runtime/` | `refactor(build): rename luthien exe folder to runtime` |
| B | Extract editor into `Luthien.lib` + introduce `IEditorHooks` | `refactor(build): extract editor into Luthien.lib` |
| C | Untrack editor-state files | `chore(repo): untrack editor state files` |
| D | Regen + docs + v2.0.0 + history + release | `chore(build): finalize target split, bump v2.0.0` |

---

## Directory Changes

### New folders
- `luthien/source/luthien/` — editor code (was `luth/source/luth/editor/`). Mirrors `luth/source/luth/`.

### Renamed
- `luthien/` exe folder → `runtime/` (`git mv`; `LuthienApp.cpp`, `Luthien.rc`, `icons/`, `resource.h` histories preserved)
- `luth/source/luth/editor/**` → `luthien/source/luthien/**` (`git mv`; 30+ files + subtrees)

### New files
- `luth/source/luth/core/EditorHooks.{h,cpp}` — `IEditorHooks` interface + `EditorHooks::Register/Get`
- `luthien/premake5.lua` — new `Luthien.lib` static-lib project
- `luthien/source/lepch.{h,cpp}` — editor PCH (includes `luthpch.h` + ImGui + Vulkan)
- `luthien/source/luthien/Bootstrap.h` — declares `InstallLuthienEditorHooks()`
- `luthien/source/luthien/EditorHooks.cpp` — `LuthienEditorHooks` impl forwarding to `Editor::*` / `ProjectLauncher::*` / `EditorSelection::*`

### Bulk rewrites
- 142 `#include "luth/editor/..."` → `#include "luthien/..."` across 45 files (perl + `binmode` for CRLF preservation)
- 26 editor `.cpp` files: `#include "luthpch.h"` → `#include "lepch.h"` (new editor PCH)

### Target layout after the epic

| Target | Kind | Links | Contents |
|--------|------|-------|----------|
| `Luth.lib`    | StaticLib  | —             | Engine only; no editor/panel code |
| `Luthien.lib` | StaticLib  | Luth          | Editor: panels, inspectors, commands, style, widgets, hook impl |
| `Luthien.exe` | ConsoleApp | Luth, Luthien | Editor application (runtime/Runtime project, targetname `Luthien`) |

### Untracked
- `runtime/editor_settings.json`, `runtime/imgui.ini`, `samples/editor_settings.json`, `samples/cache/pipeline.bin` (`git rm --cached` + new `.gitignore` patterns `editor_settings.json` and `samples/cache/`)

---

## Key Design Decisions

### `IEditorHooks` instead of a full API redesign
The structural split surfaced deep coupling: `App.cpp` drove the editor's per-frame lifecycle via 14 direct `Editor::*` / `ProjectLauncher::*` / `EditorSelection::*` call sites; `Input.cpp` queried `Editor::WantCaptureKeyboard/Mouse`; `Luth.h` (public umbrella) included `Editor.h`. A full API redesign (virtual `App` hooks, event bus, RHI layer) would have tripled scope.

Compromise: a minimal nullptr-safe `IEditorHooks` interface in `luth/core/EditorHooks.{h,cpp}` with 18 virtual methods covering the exact call-site set. `LuthienEditorHooks` (in `Luthien.lib`) forwards each call. Registration runs in `runtime/LuthienApp.cpp::CreateApp` *before* `App::App()` constructs, so the hook is live from the first `EditorHooks::Get()` call onward. A runtime-only host that skips linking `Luthien.lib` leaves the registry empty and every engine-side call nullptr-checks cleanly to a no-op.

### `EditorViewportState` snapshot instead of per-getter dispatch
`App::Run`'s per-frame block was building `CameraParams` from `Editor::GetPanel<ScenePanel>()->GetEditorCamera().GetViewMatrix()` + 6 more getters. Putting each behind a virtual call would cost 10+ dispatches per frame. Replaced with a single `IEditorHooks::GetViewportState(EditorViewportState&)` that fills a POD (view/proj/pos/near/far/IBL/selection) in one roundtrip. Engine builds `CameraParams` from it.

### Sandbox.exe descoped
Issue #78 sub-task D requested a `Sandbox.exe` target. Earlier experiments had one and it was removed as clutter. The structural goal (prove `Luth.lib` ships without editor) is enforced more cheaply:
- Physical: after B, `Luth.lib`'s `files { "source/**" }` glob excludes `luthien/`, so editor `.obj` cannot link in
- Invariant: `git grep -l 'luthien/\|Luthien' luth/source` → zero (gated in D)

A future player/standalone harness can be added when there's a real consumer.

### Layout `luthien/source/luthien/` (mirrors `luth/source/luth/`)
Alternative was `editor/source/luthien/` from `ARCH-REFACTOR-PLAN.md`. Chose the flatter form for symmetry with the engine layout — one less nesting level, and the Luthien brand is visible at repo root.

### Sub-task order reversed from issue #78
Issue ordered (A) extract editor, (C) rename `luthien/`→`runtime/`. Flipped to (A) rename first, (B) move editor in. The rename is low-risk (pure `git mv` + premake tweak); doing it first frees the `luthien/` folder name, then the high-risk ~12k-LOC move lands in an empty target. Risk-staging improved.

### VS project-name + binary-name split
New `Luthien.lib` wanted `project "Luthien"`. Old `Luthien.exe` also had that project name. Collision resolved: exe project renamed to `"Runtime"` with `targetname "Luthien"` so the output binary stays `Luthien.exe`. `startproject "Runtime"`. CI artifact paths in `.github/workflows/build.yml` updated `bin/.../Luthien/` → `bin/.../Runtime/`.

### Circular static-lib dependency avoided
Before the hooks interface, the compile-clean intermediate state had `Luth.lib` referencing `Editor::` symbols and `Luthien.lib` referencing `Luth::` symbols — a cyclic static-lib dependency that MSVC's multi-pass linker handles at exe link time. After `IEditorHooks`, the cycle is gone: `Luth.lib` has no unresolved editor symbols, `Luthien.lib` depends on `Luth`, `Luthien.exe` links both. Clean one-way.

### `imgui` refs in `luth/source` are legitimate engine infrastructure
The initial spec's D-verify included `git grep -l 'imgui' luth/source → zero matches`. This check was based on a misunderstanding. The engine legitimately uses ImGui in its render pipeline:
- `renderer/passes/ImGuiPass.cpp` — render-graph pass that composites ImGui draw data
- `renderer/RenderPipeline.cpp` — wires the ImGui pass into the graph
- `renderer/FrameDebugger.cpp` — capture-time UI
- `renderer/rendergraph/ArchivedImage.{h,cpp}` — archive-image UI
- `platform/WinWindow.cpp` — GLFW↔ImGui event bridging
- `scene/systems/RenderingSystem.cpp` — adds the ImGui pass

These are engine-level ImGui integrations, not editor code. The correct engine-cleanness invariant is `luthien/|Luthien` grep returning zero, not ImGui absence.

---

## Shrinkage / Measurement

Editor LOC moved: ~12 071 (from `ARCH-REFACTOR-PLAN.md`'s pre-epic tally). New engine-side code: ~120 LOC (`EditorHooks.h/.cpp` + `Bootstrap.h` + `LuthienEditorHooks.cpp` impl).

Post-split binary sizes (Debug x64):

| Binary | Size | Notes |
|---|---|---|
| `Luth.lib` | 294 MB | Engine only; debug symbols bloat it |
| `Luthien.lib` | 141 MB | Editor + ImGui + ImGuizmo glue |
| `Luthien.exe` | 18 MB | Runtime binary linking both libs |

Pre-epic `Luth.lib` baseline not captured — measurement deferred (would require a pre-epic rebuild).

---

## Lessons

**Editor-extraction surfaced engine→editor coupling.** `App.cpp` had 14 direct editor calls driving the editor's per-frame lifecycle; `Input.cpp` queried ImGui-capture state; `Luth.h` pulled `Editor.h`. The physical file move alone left this at include level. Adding `IEditorHooks` + nullptr-safe dispatch broke it cleanly with minimal API surface. A virtual-method `App` redesign would have been 5× the work — the hook interface is the right scope for a structural epic.

**Git rename threshold can miss small files.** `Command.h` had 10 lines total, 5 of which were `#include "luth/editor/commands/..."` lines that the perl rewrite changed. That's a 50%+ edit by git's default similarity metric, so git showed the move as `delete + create` instead of a rename. Larger files with the same 5-line change detect cleanly. `--find-renames=30%` on `git log` / `git diff` lowers the threshold if needed.

**Bulk rewrites with perl + binmode.** Same recipe as `arch-cleanup`: `perl -i -pe 'BEGIN{binmode(ARGV);binmode(STDOUT);} s|old|new|g'` preserves CRLF on Windows. 168 rewrites in this epic (142 include paths + 26 PCH switches) all clean — no stray LF/CRLF flips in `git diff`.

**ImGui is engine infrastructure, not editor-exclusive.** The spec's initial assumption was that ImGui only lives in editor code. Real dependency is in 6 engine files across rendergraph, frame debugger, and platform. The correct editor-cleanness invariant is `luthien/|Luthien` grep, not `imgui` grep.

**Preserved a functioning editor throughout.** Every commit landed build-clean with full editor parity. No multi-commit broken state — sub-staging (B1/B2/B3 contingency) wasn't needed because the hooks interface was designed before any engine code was rewritten, keeping each commit atomic.

---

## Build Verification

- 4 work commits (plus the kickoff `docs(epic): add arch-target-split spec`)
- All sub-tasks build Debug x64 clean at HEAD; warnings are pre-existing noise (`C4267` `size_t→uint32_t`, `C4244` `chrono::rep`, `C4996` `getenv`, `LNK4006` Vulkan import-descriptor duplicates)
- 3-project solution (`Luth`, `Luthien`, `Runtime`) regenerates cleanly via `scripts\setup\setup_windows.bat`
- `git grep -l 'luthien/\|Luthien' luth/source` → zero matches ✓
- `git grep -l 'Editor::' luth/source` → zero matches ✓
- `Luthien.exe` launches, loads project, renders scene, all panels functional
- `.gitignore` covers per-user editor-state drift (`editor_settings.json`, `*imgui.ini`, `samples/cache/`)
- CLAUDE.md is currently untracked; its "Targets" and "Source Structure" bullets were updated locally but not committed this epic — track whenever desired
