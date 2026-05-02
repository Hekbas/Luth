# v2.9.3 — editor-job-pump

**Date:** 2026-05-02
**Commits:** 2 (on `feat/editor-job-pump`)
**Issue:** [#112](https://github.com/Hekbas/Luth/issues/112)
**Series:** AAA editor rework, effort 4 of 8

---

## Overview

Adds `Luth::MainThreadPump` — a static facade with `Post(Callback)` / `Drain()` /
`PendingCount()` — that lets editor-side `JobSystem::Execute` async work hop back
to main for ImGui-touching follow-up. No real consumer ships in this effort; the
pump is the foundation that `editor-autosave` (v2.9.4) and `editor-thumbnails`
(v2.9.5) will build on.

The shape is a deliberate clone of the v2.9.1-hardened `EventBus`: static
singleton, `std::queue<Callback>` under `std::mutex`, swap-and-drain on the
main thread, debug-only thread-assertion latched on the first `Drain()`,
per-callback `try/catch` so a thrown callback can't strand pending work, and
`MemoryTracker::Category::Editor` accounting outside the lock.

Tag-only release. The pump is invisible until consumers exercise it.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | `MainThreadPump.{h,cpp}` + `App::Run` drain hook at L178 | [`bbffdb3`](../../../../commit/bbffdb3) |
| B | Wrap-up: docs + version bump + history | this commit |

Two commits, deliberately under-fragmented. The Plan agent suggested splitting
the App.cpp drain hook into its own commit (~5 LOC) for bisect granularity.
Rejected: the pump impl and its activation site land atomically; if the drain
wires wrong, the smoke test catches it immediately, no bisect window matters.

---

## Architectural decisions

### Storage: `std::queue` + `std::mutex`, not `MPMCQueue`

`MPMCQueue<T,Capacity>` (`luth/jobs/MPMCQueue.h`) is the engine's lock-free
bounded transport, used inside `JobSystem` for the global high-priority queue.
Considered and rejected here:

- **Bounded capacity is wrong shape.** A thumbnail invalidation when a folder
  opens can post 200 callbacks at once; an autosave failure could burst error
  callbacks. Picking a power-of-2 capacity a priori means picking a number that
  silently drops work under a burst, or oversizing for the worst case.
- **`std::queue` matches the EventBus precedent.** EventBus's `m_QueueLock` is
  a `std::mutex`; the cornerstone "no `std::mutex` on hot paths" applies, but
  the pump is edge-frequency (autosave fires every N seconds; thumbnail bursts
  fire on visibility), not per-frame work. Same shape, same trade.

If profiling later shows lock contention is a problem, the migration path is
swap to `MPMCQueue` with a paged-overflow fallback — but that's borrowing
trouble before there's a real load.

### Why not generalize `AssetManager::s_UploadQueue`?

`AssetManager::Update()` (`luth/resources/AssetManager.cpp` L292-356) drains a
`std::vector<PendingUpload>` and looks superficially like a main-thread pump.
It is not. Reading the loop:

- Typed dispatch on `upload.Type` (`Material`, `Model`, ...) — different
  finalisation paths per asset type, not opaque callbacks.
- Mid-iteration recursion into `LoadAsync` (Material textures, Model anim
  clips) — enqueues *new* upload requests that can't be processed in the same
  drain pass.
- Coupled to `s_AssetMutex` and `s_LoadingAssets` housekeeping.
- Coupled to `UploadContext::DrainPendingBinds()` at L305 — a different
  ordering constraint (GPU-fence retirement).

Erasing this into `MainThreadPump::Post([data]{ FinalizeAsset(...); })` would
either spread the typed branching across the post sites (ugly and duplicated),
or hide it behind opaque lambdas (loses the ability to reason about the upload
pipeline as a coherent unit).

The asset upload queue is a *typed pipeline stage*, not a generic callback
hop. Two queues, two purposes — `MainThreadPump` for opaque worker→main
callback erasure, `AssetManager::s_UploadQueue` for asset finalisation. They
share a shape but not semantics.

### Drain placement at `App.cpp:178`

Inserted immediately after `EventBus::ProcessEvents(BusType::MainThread)` at
L177 and before any of `EditorHooks::BeginFrame()`, `OnUpdate()`, or
`AssetManager::Update()`:

```cpp
EventBus::ProcessEvents(BusType::MainThread);
MainThreadPump::Drain();
```

Reasoning:

- L177 is the canonical "main-thread mailbox drain" point. Both `EventBus` and
  the pump are queues of side effects produced by other threads, drained on
  main. Keeping them adjacent makes the read story obvious and the relative
  ordering deterministic.
- Draining *before* `BeginFrame()` means callbacks see the previous frame's
  ImGui as ended — they should mutate engine/editor state, not call ImGui.
  That's the right contract: the pump moves work from worker to main, not into
  the middle of ImGui's frame.
- Pre-`m_ProjectLoaded` branch so launcher-screen consumers can post.

A double-drain pattern (drain at L177 and again before render) was considered
and rejected — V2 main-thread starvation isn't a risk at this frequency, and
the second drain would just duplicate behaviour.

### V1-V6 hazard mapping

| Hazard | Status | Mitigation |
|---|---|---|
| V1 lock contention | Edge-frequency cold path | `std::mutex` acceptable per EventBus precedent |
| V2 main-thread starvation | Drain on main | Drain-all on swap (EventBus shape); add bound later if needed |
| V3 ImGui from worker | Worker-side `Post` doesn't touch ImGui | Documented contract: workers may not, callbacks may |
| V4 lost wakeup | N/A | Drain every frame, no sleep/wait protocol |
| V5 sub-job context thrash | N/A | Callbacks don't inline-dispatch jobs |
| V6 GPU↔allocator deadlock | N/A | `Memory::Category::Editor`, not `FrameTagged` |

### Re-entrancy contract

A callback that calls `Post(...)` during its own dispatch enqueues onto
`s_Queue`, not the local `processing` queue that the swap-and-drain owns.
The new callback fires in the *next* frame's drain. Same contract as
`EventBus::ProcessEvents`. Documented inline so consumers don't expect
synchronous re-dispatch.

### Memory accounting

Each `Post` records `sizeof(Callback)` against `Memory::Category::Editor`;
`Drain` pairs each pop with `RecordFree`. The pattern matches `EventBus`'s
`EventDeleter` shape — track the wrapper size; the lambda's heap capture (if
any) is caught by Tracy's global `new` hook. ProfilerPanel's Editor row
returns to baseline within one frame after a burst drains.

---

## Files & locations

### New
- `luth/source/luth/jobs/MainThreadPump.h` — public API + thread-safety doc.
- `luth/source/luth/jobs/MainThreadPump.cpp` — `std::queue` + `std::mutex`; thread-assert; per-callback `try/catch`; tracked allocations.

### Modified — engine
- `luth/source/luth/core/App.cpp` — `+1` include, `+1` line at L178: `MainThreadPump::Drain()` after `EventBus::ProcessEvents`.
- `luth/source/luth/core/Version.h` — bumped to v2.9.3.

### Modified — docs
- `docs/development/ROADMAP.md` — v2.9.3 row in completed table.
- `CLAUDE.md` — Current Progress block updated (untracked).
- `docs/development/history/v2.x/editor-job-pump.md` — this file.

---

## Build Verification

2 atomic commits on `feat/editor-job-pump`; every commit builds Debug x64
clean (pre-existing C4996 / C4244 baseline only).

Smoke test (synthetic, temp scratch not committed):

- Posted 100 callbacks from `JobSystem::Execute` worker fibers, all 100
  delivered to main within 1-2 frames; no thread-mismatch assert; no
  thread-of-origin surprises.
- `MemoryTracker::Category::Editor` returns to baseline after the drain.
- Re-entrancy confirmed: posting from main inside a callback runs next frame,
  not synchronously.
- Editor shutdown clean — no use-after-free, no leaks. The pump's static state
  outlives all consumers, so teardown order is naturally correct.

Real verification deferred to v2.9.4 (`editor-autosave`) and v2.9.5
(`editor-thumbnails`) where consumers exercise the pump end-to-end.

Closes #112.
