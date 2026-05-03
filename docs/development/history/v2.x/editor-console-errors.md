# v2.9.2 — editor-console-errors

**Date:** 2026-05-02
**Commits:** 5 (on `feat/editor-console-errors`)
**Issue:** [#111](https://github.com/Hekbas/Luth/issues/111)
**Series:** AAA editor rework, effort 3 of 8

---

## Overview

Bundles two pillars from the editor-aaa plan:

- **ConsolePanel + `Log::AddSink`** — engine logs flow through a new `ILogSink`
  interface; the `ConsolePanel` registers itself as a sink, hops to main thread via
  `EventBus::LogEntrySignal`, and renders a filterable / searchable list.
- **Per-panel error boundaries on `OnDraw`** — `Editor::DrawPanelGuarded` mirrors the
  existing `GatherJobThunk` catch contract on the draw path. `m_CrashStreak >= 3`
  flips `m_Crashed`; the panel is replaced by a placeholder window with a manual
  `Reset` button. Stack-trace dump (Win32 DbgHelp) on every catch.

Tag-only release. The console is end-user-visible but the series is mid-flight; we
ship a milestone Release at the end.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | `ILogSink` + `Log::AddSink/RemoveSink` + `ForwardingSink` (spdlog `base_sink`) | [`42bff3d`](../../../../commit/42bff3d) |
| B | `LogEntrySignal` + `ConsolePanel` (Panel + ILogSink, capped deque, level filter, search) | [`9d281c1`](../../../../commit/9d281c1) |
| C | `Editor::DrawPanelGuarded` + `DrawCrashedPlaceholder` + `StackTrace::Capture` (DbgHelp) | [`346a9df`](../../../../commit/346a9df) |
| D | Smoke-test fallout: clipper, toggles, sink lifetime | [`fde99e0`](../../../../commit/fde99e0) |
| E | Wrap-up: docs + version bump + history | this commit |

Each sub-task builds Debug x64 clean. Pre-existing C4996 (`getenv`/`strncpy`) and
C4244 (`chrono::rep` conversion) warnings unchanged.

---

## Architectural decisions

### Bus indirection vs SpinLock-guarded ring

`ILogSink::OnLogEntry` fires from any thread (worker fibers via `LH_CORE_*` inside
jobs, IOThread, main). Two designs were available:

1. **Direct ring on the panel** — `ConsolePanel` owns a SpinLock-guarded vector;
   sink callback appends under the lock; `OnDraw` reads under the same lock.
2. **EventBus indirection** — sink enqueues `LogEntrySignal`; main drains during
   `EventBus::ProcessEvents`; handler appends to `m_Entries` on main only.

Design 2 was already chosen during the v2.9.0 review (called out in
`history/v2.x/editor-foundation.md` §"Console ring buffer: replaced by EventBus").
The decision rested on log emission being edge-frequency in steady state — bus
overhead is acceptable, and the indirection naturally serialises the producer-side
hop without us reasoning about a new SpinLock.

`m_Entries` is mutation-stable across `OnGather`: `EventBus::ProcessEvents` runs on
main at `App::Run` L177 *before* `Editor::Render` dispatches gather (L227). Any
sink callback firing from a worker fiber during gather just enqueues — it doesn't
dispatch. Drain is next-frame. So the panel's vector is single-writer-on-main
across the frame boundary; no lock needed.

The only concurrent state is the engine-side sink list (`Log::s_Sinks` in
`Log.cpp`). `AddSink/RemoveSink` and `ForwardingSink::sink_it_` race; covered by a
`Luth::SpinLock`. The cornerstone "no `std::mutex` on hot paths" applies — log
emission from a worker fiber is hot.

### Re-entrancy hazard

spdlog's `base_sink<Mutex>` uses a non-recursive `std::mutex`. If an `ILogSink`
implementation called `LH_CORE_*` from `OnLogEntry`, control would re-enter
`base_sink::log` and deadlock. `ConsolePanel::OnLogEntry` only calls
`EventBus::Enqueue`, which doesn't log. The contract is documented on `ILogSink`'s
declaration as `MUST NOT call LH_CORE_*`.

### Why `ConsoleSnapshot` is a placeholder

The foundation pattern is "OnGather collects, OnDraw reads frozen view." For this
panel, the collection is just per-frame filter evaluation against `m_Entries`,
which is on-main, mutation-stable through the frame, and small (≤1024 entries).
Filtering on a fiber would copy-out hundreds of `LogEntry` strings into the
gather scratch — net loss. `OnDraw` filters live; the `ConsoleSnapshot`
placeholder keeps the panel on the lifecycle for consistency. Future polish can
move filter state if search becomes expensive.

### `ImGuiListClipper` removed

The first cut used `ImGuiListClipper` to keep the per-frame draw cost flat across
N entries. Smoke test surfaced an `IM_ASSERT` on `ItemsHeight > 0`: the clipper
probes height by measuring the first row's cursor advance, but the panel's filter
loop `continue`s past hidden levels and search misses — so the first item is
often a zero-advance row that breaks the probe. Compounded by `TextWrapped`
producing variable row heights, the clipper was the wrong tool. Sub-task D drops
it; ImGui still GPU-clips off-screen geometry, and the per-row CPU cost at
cap=1024 is benign in practice. If profiling shows otherwise, the path forward is
pre-filter into a `std::vector<size_t>` of visible indices + single-line `Text`
+ clipper — but that wasn't worth doing eagerly.

### Unity-style level toggles

The first toolbar layout was a horizontal strip of `Checkbox + colored Icon`
pairs. Smoke-test feedback was that the `Trc/Dbg/Inf/Wrn/Err/Crt` short labels +
checkboxes felt cluttered and weren't aligned to the right edge of the toolbar.
Sub-task D replaces the pairs with toggleable icon buttons: the icon itself is
the button, click toggles, ImGui `SetTooltip` names the level. Cluster is
right-aligned by computing remaining content width and seeking the cursor.
ImGui has no native toggle button — the helper `LevelToggle` re-skins
`ImGui::Button` via the four state colours plus the text colour, parametrised
by the `tint` of the level (the same colour the row text uses).

### Sink lifetime — Editor lifecycle gap closed

Second smoke-test crash: post-shutdown access violation in `ForwardingSink::sink_it_`.
Root cause split across two surfaces:

1. **`Editor::Shutdown` never invoked `Panel::OnShutdown`.** A foundation-era gap
   that didn't matter while no panel registered with engine subsystems. Once
   `ConsolePanel` registered itself with `Log::AddSink`, the gap turned into
   a use-after-free: `s_Panels.clear()` destroyed the panel, then the trailing
   `LH_CORE_INFO("Editor system shutdown completed")` walked the still-registered
   stale pointer through `ForwardingSink`. Sub-task D adds the missing
   `for (auto& panel : s_Panels) panel->OnShutdown()` before clearing.
2. **`ConsolePanel`'s destructor wasn't a backstop.** Even with the lifecycle
   call wired, future panels could still leak a registration if shutdown short-
   circuited. RAII via `~ConsolePanel()` calling `Log::RemoveSink(this)` +
   `EventBus::Unsubscribe` provides defence in depth. Both APIs are idempotent —
   safe to call after `OnShutdown` already ran.

### Crashed-panel placeholder reuses the panel's window ID

`DrawCrashedPlaceholder` calls `panel->BeginWindow(panel->GetWindowID())` — same
ImGui ID as the live panel. Docking layout, position, size all persist across
the crash → reset cycle. A separate `"<id> (crashed)"` window would have created
an undocked replacement that the user has to redock, which is a worse UX than
the original goal: keep the rest of the editor responsive while one panel goes
dark.

`m_Visible` introspection still flows through `BeginWindow` so the crashed panel
contributes to the gather-skip dispatch (no point dispatching gather to a panel
already known crashed; and once reset, gather resumes next frame). `m_Crashed` is
checked first in the Render loop — crashed panels never go through
`DrawPanelGuarded`, only the placeholder.

### Stack trace via DbgHelp, not `<stacktrace>`

C++23 `std::stacktrace` is the natural choice but isn't available in our MSVC
target (C++20). DbgHelp is built into Windows (`dbghelp.lib` already linked from
the foundation v2.0.0 era, see `luth/premake5.lua:65`); `CaptureStackBackTrace` +
`SymFromAddr` + `SymGetLineFromAddr64` produces the same output without a new
dep. `SymInitialize` is process-global, called once via `std::call_once`.

DbgHelp's symbol-resolution APIs are documented as single-threaded; resolution
is serialised behind a `Luth::SpinLock`. Frame *capture* (`CaptureStackBackTrace`)
is documented as thread-safe and runs outside the lock.

### Pre-existing `Pillar 5` placeholder comments removed

The foundation effort planted three forward-reference comments that violated the
new style bar enforcement: `Editor.h:82` (`m_Crashed` field), `Editor.h:169`
(`GatherJobThunk` decl), `Editor.cpp:334` (gather catch body). All three were
factual placeholders for "this will be extended in editor-console-errors v2.9.2."
With this effort live, the placeholders become factually wrong (already
extended) and structurally noisy (the comment talks about a thing that's right
there). All three replaced with terse what-it-is comments.

---

## Files & locations

### New
- `luth/source/luth/core/diagnostics/StackTrace.h` — `Capture` + `LogStackTrace` API.
- `luth/source/luth/core/diagnostics/StackTrace.cpp` — Win32 DbgHelp impl, Linux fallback returns empty.
- `luthien/source/luthien/panels/ConsolePanel.h` — `ConsolePanel` declaration (`Panel` + `ILogSink`) with explicit dtor.
- `luthien/source/luthien/panels/ConsolePanel.cpp` — implementation; level icons + colors; right-aligned `LevelToggle` icon buttons; live-filter list (no clipper); RAII sink cleanup in dtor.

### Modified — engine
- `luth/source/luth/core/diagnostics/Log.h` — `LogLevel` / `LogEntry` / `ILogSink` types; `Log::AddSink/RemoveSink` API.
- `luth/source/luth/core/diagnostics/Log.cpp` — internal `ForwardingSink` (spdlog `base_sink`) fanning to registered `ILogSink`s under `SpinLock`; registered alongside stdout + file sinks in `Log::Init`.
- `luth/source/luth/core/Version.h` — bumped to `v2.9.2`.

### Modified — editor (luthien)
- `luthien/source/luthien/events/EditorSignals.h` — new `LogEntrySignal` carrying `LogEntry` by value.
- `luthien/source/luthien/Editor.h` — `DrawPanelGuarded` / `DrawCrashedPlaceholder` decls; cleaned forward-reference placeholder comments.
- `luthien/source/luthien/Editor.cpp` — `DrawPanelGuarded` + `DrawCrashedPlaceholder` impl; `GatherJobThunk` catch dumps stack trace + adds non-std catch path; Render loop routes through guards; `Shutdown` invokes `OnShutdown` on each panel before clearing; `AddPanel(new ConsolePanel())` in `InitPanels`.

### Modified — docs
- `docs/development/ROADMAP.md` — v2.9.2 row in completed table.
- `CLAUDE.md` — Current Progress block updated.
- `docs/development/history/v2.x/editor-console-errors.md` — this file.

---

## Build Verification

5 atomic commits on `feat/editor-console-errors`; every commit builds Debug x64
clean. Smoke test surfaced three issues, all consolidated into sub-task D before
merge: `ImGuiListClipper` `IM_ASSERT`, toolbar UX feedback (checkbox + icon
pairs replaced with toggleable icon buttons, right-aligned), and a shutdown
use-after-free where post-clear `LH_CORE_INFO` walked a freed `ConsolePanel`
through `ForwardingSink`. Re-run smoke test confirms:

- Console panel docks alongside the others; `LH_CORE_INFO/WARN/ERROR` lines
  appear with appropriate colours and level icons.
- Trace / Debug toggles default off — stays readable through asset-load bursts.
- Search filters by case-insensitive substring against `LogEntry::message`.
- Clear empties the deque immediately; ring auto-trims at 1024 entries.
- Auto-scroll latches to bottom on each new arrival; toggle disables.
- Force-throw injection in a panel's `OnDraw` produces three error logs +
  stack traces, fourth frame replaces with the crashed-placeholder, `Reset`
  brings the panel back.
- Force-throw injection in `OnGather` produces the same flow via the gather
  catch.
- Editor shutdown is clean — `LH_CORE_INFO` at the end of `Editor::Shutdown`
  no longer crashes through a stale `ConsolePanel` sink.
- No new Vulkan validation errors.

Closes #111.
