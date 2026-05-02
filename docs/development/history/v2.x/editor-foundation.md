# v2.9.0 — editor-foundation

**Date:** 2026-05-01
**Commits:** 12 (on `feat/editor-foundation`) + 1 ROADMAP reorder direct on main
**Issue:** [#109](https://github.com/Hekbas/Luth/issues/109)
**Series:** AAA editor rework, effort 1 of 8 — [`editor-foundation` v2.9.0 → `editor-workspaces` v2.9.7]

---

## Overview

First effort of the AAA editor rework series. Replaces the bare `OnInit`/`OnRender` `Panel` contract with a Gather → Draw lifecycle that mirrors the engine's `Game(N) | Render(N-1)` dispatch pattern at smaller scale. Panel data collection runs in parallel on worker fibers via `JobSystem::Execute`; ImGui submission stays on main thread reading frozen, immutable per-panel snapshots. Eight subsequent efforts (v2.9.1 – v2.9.7) build on this foundation: signal bus, console + error boundaries, job pump, autosave + crash recovery, thumbnail cache, live-preview undo, workspaces.

This effort is intentionally **structural, not perf-focused**. The lifecycle is in place and every panel flows through it; the actual work-shifting from `OnDraw` into `OnGather` (where the parallelism payoff lives) is left as a follow-on polish opportunity per panel — explicitly called out in each migration commit. Today's `OnGather` for most panels is a placeholder that creates an empty fragment; tomorrow's polish epics flesh them out as the surrounding pillars (signal bus, job pump) make richer gather work practical.

Editor behavior is functionally identical to v2.8.14. No user-visible change; this is the API generational shift.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | `Panel` base extension — new hooks, `m_GatherAlloc`, introspection fields | [`ccc62f2`](../../../../commit/ccc62f2) |
| B | `EditorSnapshot` + `EditorSnapshotBuilder` (header-only, panel-local) | [`ac538e0`](../../../../commit/ac538e0) |
| C | `Editor::Render` gather/draw bridge — `JobSystem::Execute` dispatch + `WaitForCounter` + snapshot assembly | [`6136da6`](../../../../commit/6136da6) |
| D | HierarchyPanel migration | [`de52414`](../../../../commit/de52414) |
| E | InspectorPanel + 6 inspectors migration | [`a081ddc`](../../../../commit/a081ddc) |
| F | Scene + Game panels migration | [`2bef412`](../../../../commit/2bef412) |
| G | Project + Resource panels migration | [`82607e0`](../../../../commit/82607e0) |
| H | Render + Profiler + History panels migration | [`ed7a8fc`](../../../../commit/ed7a8fc) |
| I | FrameDebuggerPanel migration | [`53cbcab`](../../../../commit/53cbcab) |
| J | Panel introspection — `Panel::BeginWindow` updates `m_Visible`/`m_Focused`/`m_Docked` | [`df36e73`](../../../../commit/df36e73) |
| K | Remove legacy `OnRender` from `Panel` base; strip per-panel `UsesNewLifecycle` overrides | [`c24d66e`](../../../../commit/c24d66e) |
| L | Wrap-up: docs + version bump + history | this commit |

Each sub-task builds Debug x64 clean. Sub-tasks A–C land the infrastructure; D–I are mechanical per-panel migrations on the bridge; J adds the visibility introspection that Editor's gather loop already consults; K cleans up the bridge once every panel is on the new path.

---

## Architectural Decisions

### Allocator: per-panel `LinearAllocator`, not `TaggedPageAllocator`

The original Plan-agent design proposed a per-frame editor tag on the global `TaggedPageAllocator`, reasoning "cross-fiber per-frame data." That decision was reversed during Phase 3 review against [arch/memory.md](../../arch/memory.md) and [arch/fiber-system.md](../../arch/fiber-system.md):

- `TaggedPageAllocator::FreeTag(tag)` only scans `m_UsedPages`; pages held in a fiber's active `JobContext.CpuCache` are invisible to it (verified in [`TaggedPageAllocator.cpp`](../../../../luth/source/luth/memory/TaggedPageAllocator.cpp) `FreeTag` impl).
- The engine's existing pattern (V6 game/render frame tags driven from `VulkanBackend::AcquireImage`) only works because every fiber participates in every frame's stage dispatch — they all naturally rotate their `CurrentTag` and flush their cache page back into `m_UsedPages` before any `FreeTag` runs.
- Editor `OnGather` only runs on a subset of fibers each frame (one job per visible panel). Fibers that never pick up gather work would accumulate stale `EditorFrameTag(N-K)` pages indefinitely.

Per-panel `LinearAllocator(64*1024)` sidesteps the issue entirely — exactly the arch-doc use case ("per-frame, per-thread; bump until `Reset()`. Frame-temp data inside a single fiber's stack of work"). Each panel's `OnGather` is a single fiber's stack; no sharing, no sync. `Reset()` rewinds without freeing pages, so subsequent frames reuse the same backing memory. ~10 panels × 64 KB ≈ 640 KB resident — trivial.

The trade-off (which `TaggedPageAllocator` would have offered): no cross-frame fenced reclamation. Editor doesn't need it — gather and draw both run inside a single `App::Run` iteration on the same thread sequence; no GPU fence, no cross-frame holdover.

### Console ring buffer: replaced by `EventBus` dispatch (deferred to v2.9.2)

The Phase 2 plan included a custom MPSC ring buffer for the (forthcoming) `ConsolePanel`'s log sink. Phase 3 review determined it was over-engineered for the actual rate of log emission (edge-frequency: errors, lifecycle events, asset-reload notifications — not per-frame, not per-job). The plan was revised to bounce log entries through the existing `EventBus::BusType::MainThread` and let the panel drain on main via `ProcessEvents`. The actual `ConsolePanel` lands in v2.9.2 (`editor-console-errors`); this effort just preserves the design call in the plan file.

### Bridge mechanism for incremental panel migration

A pragmatic bridge let each panel migrate in its own commit while keeping the editor functional throughout:

- Sub-task A added `OnGather`/`OnDraw`/`OnEvent`/`OnShutdown` as default-impl on `Panel` alongside the still-existing `OnRender`. A `UsesNewLifecycle()` virtual returned `false` by default.
- Sub-task C rewired `Editor::Render`: gather phase only dispatches for panels where `UsesNewLifecycle()` returns `true`; draw loop calls `OnDraw(snapshot)` for migrated panels and `OnRender()` for legacy.
- Sub-tasks D–I migrated each panel one at a time, each commit overriding `UsesNewLifecycle() → true` and splitting its `OnRender` into a placeholder `OnGather` + a copy as `OnDraw`. Build clean after every commit. Editor functionally identical.
- Sub-task K removed `OnRender` and `UsesNewLifecycle` from the base + per-panel overrides once the migration was complete, simplifying `Editor::Render`.

The bridge sentinel made each migration commit a drop-in — no spooky action across panels, no all-or-nothing diff. This is the same incremental pattern v2.7.x editor-review used, scaled up for a 12-commit effort.

### ECS quiescence — known one-frame-stale window

`OnGather` reads ECS during a window where async asset-loaders may complete and write the registry. The `App::Run` loop fires `AssetDatabase::ProcessPendingChanges` (line 220) *before* `Editor::Render` (line 227) — synchronous file-watch callbacks complete first. But async asset-load completions on worker fibers can write the ECS during gather. Result: a one-frame stale snapshot if an asset finishes loading mid-gather.

This is a pre-existing engine concern (no "ECS writes only on main" rule today), not introduced by the rework. Documented as accepted: an interactive editor can tolerate one-frame staleness; the editor rebuilds next frame. A future engine epic could enforce ECS-mutation-on-main via a deferred queue, eliminating the hazard for editor *and* `GameStage` simultaneously — out of scope here.

### Work-shifting deferred to follow-on polish

Most panels' `OnGather` is a placeholder for v2.9.0. The lifecycle exists; the work-shift hasn't happened. The decision was deliberate:

- Each panel's "real" gather is its own design problem. ProjectPanel's `BuildDirectoryTree` (recursive filesystem walk) and `UpdateSearchResults` (lowercase + substring filter on every keystroke) are the obvious candidates per the pre-rework Tracy capture. ProfilerPanel's stat aggregation (frame-time history rotate, MemoryTracker snapshot, JobSystem stats, GPU memory) is another.
- Migrating 9 panels with their `OnGather` content fully fleshed out in one effort would have been a 30+ commit branch with much higher behavior risk.
- The structural lifecycle is the leverage. With it in place, each future polish commit is a self-contained "shift X panel's work into gather" diff. Easy to review, easy to verify, easy to revert if behavior regresses.

Comments in each panel's migration commit and `OnGather` body explicitly call out where the work-shift opportunities live, so future maintainers (or follow-on epics) have a roadmap.

### `Panel::BeginWindow` for introspection

ImGui's `Begin` returns visibility; `IsWindowFocused`/`IsWindowDocked` query post-Begin state. Rather than adding `Editor::UpdatePanelIntrospection` to read previous-frame state via `ImGui::FindWindowByName`, sub-task J introduced a `Panel::BeginWindow(name, flags)` helper that wraps `ImGui::Begin` and updates `m_Visible`/`m_Focused`/`m_Docked` from the returned state. Each panel's `OnDraw` migrated from direct `ImGui::Begin(...)` to `BeginWindow(...)` — single line per panel.

The visibility flows back into the next frame's gather-dispatch loop: invisible panels skip `OnGather`, accepting the spec-acknowledged one-frame stale on visibility resume. No `UpdatePanelIntrospection` needed; the panel does the right thing inline during its own draw.

---

## Files & locations

### New
- `luthien/source/luthien/EditorSnapshot.h` — `EditorSnapshot` + `EditorSnapshotBuilder` (header-only, no .cpp needed).

### Modified — engine
- `luth/source/luth/core/Version.h` — bumped to v2.9.0.

### Modified — editor (luthien)
- `luthien/source/luthien/Editor.h` — replaced `Panel` base; added forward decls; `friend class EditorSnapshotBuilder`; `Panel::BeginWindow` helper; `Editor::GatherJobThunk` declaration.
- `luthien/source/luthien/Editor.cpp` — new `Render` body with gather phase + snapshot assembly; `GatherJobThunk` impl with try/catch + crash-streak bump; new include of `EditorSnapshot.h` + `JobSystem.h`.
- `luthien/source/luthien/panels/HierarchyPanel.{h,cpp}` — `HierarchySnapshot` POD; OnGather/OnDraw split; `BeginWindow` migration.
- `luthien/source/luthien/panels/InspectorPanel.{h,cpp}` — `InspectorSnapshot` POD; same split.
- `luthien/source/luthien/panels/ScenePanel.{h,cpp}` — `SceneViewportSnapshot`; same split.
- `luthien/source/luthien/panels/GamePanel.{h,cpp}` — placeholder `GameViewportSnapshot`; same split.
- `luthien/source/luthien/panels/ProjectPanel.{h,cpp}` — placeholder `ProjectSnapshot`; same split.
- `luthien/source/luthien/panels/ResourcePanel.{h,cpp}` — placeholder `ResourceListSnapshot`; same split.
- `luthien/source/luthien/panels/RenderPanel.{h,cpp}` — placeholder `RenderSettingsSnapshot`; same split.
- `luthien/source/luthien/panels/ProfilerPanel.{h,cpp}` — placeholder `ProfilerSnapshot`; same split.
- `luthien/source/luthien/panels/HistoryPanel.{h,cpp}` — placeholder `HistorySnapshot`; same split.
- `luthien/source/luthien/panels/FrameDebuggerPanel.{h,cpp}` — placeholder `FrameDebuggerSnapshot`; same split.

### Modified — docs
- `docs/development/ROADMAP.md` (committed direct to main as `e937a31`) — reordered Planned Epics: 8 editor-aaa entries before jolt; jolt + downstream renderer arc shifted +1 MINOR.
- `docs/development/history/v2.x/editor-foundation.md` (this file).

---

## Build Verification

- 12 atomic commits on `feat/editor-foundation`; every commit builds Debug x64 clean with no new warnings.
- Pre-existing C4996 (`getenv`/`strncpy`) and C4244 (`chrono::rep` conversion) warnings unchanged.
- No new Vulkan validation errors.
- Editor functionally identical to v2.8.14 per smoke test (Hierarchy/Inspector/Scene/Game/Project/Profiler/FrameDebugger/History/Resources/Render).

Closes #109.
