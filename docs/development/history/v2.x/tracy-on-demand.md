# v2.8.3 — tracy-on-demand

**Date:** 2026-04-25
**Commits:** 1 (on `fix/tracy-on-demand`)
**Issue:** [#30](https://github.com/Hekbas/Luth/issues/30)

---

## Overview

Hotfix for two memory leaks that turned out to share a single root cause: Tracy's per-thread serial queue was accumulating zone, frame-mark, and (post-v2.8.2) global allocation events even when no Tracy GUI client was connected to drain them. Defining `TRACY_ON_DEMAND` in the build makes every Tracy macro short-circuit to a no-op while `Profiler::IsConnected()` is false — the queue stays empty, no leak.

Two symptoms collapsed into one fix:

- **~10 MB/s RSS growth on the ProjectLauncher screen** — fresh regression introduced by v2.8.2's `GlobalNewDelete.cpp`. Once every `operator new`/`operator delete` started feeding `TracyAlloc`/`TracyFree`, the launcher's per-frame string churn (recent-projects list, ImGui Demo, Luth Metrics panel) hit the queue at thousands of events per frame.
- **~0.2 MB/s RSS growth in-game at 240 fps** — pre-existing leak from issue #30 (opened 2026-04-05). Predates the global hooks; was driven by zone/frame-mark events alone. Three weeks of investigation hypotheses on the issue (MemoryTracker, VMA, descriptor sets, render graph frame allocator, triple-buffered cleanup) all aimed at the wrong layer because Tracy's own internal buffers don't show up in `MemoryTracker` (which only sees explicit `LH_NEW`/`LH_DELETE`).

The diagnosis flipped after noticing the leak *only* happens with no Tracy client connected — a Tracy capture would have been useless because the act of capturing is what stops the symptom. From there the fix was an easy build edit.

---

## Root cause

Without `TRACY_ON_DEMAND`, Tracy macros queue events unconditionally:

```cpp
static tracy_force_inline void SendFrameMark( const char* name, QueueType type )
{
#ifdef TRACY_ON_DEMAND
    if( !GetProfiler().IsConnected() ) return;  // ← absent in pre-v2.8.3 builds
#endif
    auto item = QueueSerial();
    // ... write event into per-thread serial queue
}
```

The Profiler worker thread drains those queues only when a client is connected. With no client, the queues grow forever. Bytes leaked = (events/frame) × (frames/sec) × (per-event payload). Roughly:

- Launcher (60 fps, ~3000 alloc events/frame post-v2.8.2): ~10 MB/s.
- In-game (240 fps, tighter alloc patterns + zones + frame marks pre-v2.8.2): ~0.2 MB/s.

`MemoryTracker` reads balanced because the leaked bytes live inside Tracy's own internal buffers — outside the `LH_NEW`/`LH_DELETE` path the tracker observes.

---

## Fix

Add `"TRACY_ON_DEMAND"` to the Debug and Release define lists across all four premake files that compile against Tracy:

| File | Configurations |
|---|---|
| `luth/extern/premake5-tracy.lua` | Debug, Release |
| `luth/premake5.lua` | Debug, Release |
| `luthien/premake5.lua` | Debug, Release |
| `runtime/premake5.lua` | Debug, Release |

Dist already excludes Tracy entirely, so no change there.

---

## Behavioral change

With `TRACY_ON_DEMAND` enabled, Tracy operates in deferred-capture mode: events are not queued until a profiler client connects. Practically:

- Running without the Tracy GUI: zero memory overhead from Tracy macros. RSS flat.
- Running with the Tracy GUI: trace begins from the moment of connection (no retroactive history). Identical to before for any session where the user starts the GUI client first and then exercises the engine — the standard workflow.

The trade-off (no pre-connection history) is acceptable for a development-only feature. Tracy is excluded from Dist builds anyway.

---

## Build verification

Debug x64 builds clean from regenerated solution. No new warnings. `TracyClient.cpp` recompiles with the new define. Behavioral verification per `Luthien.exe` smoke test:

1. Launcher idle (no Tracy client) → RSS flat for 60+ s. Was: ~10 MB/s growth.
2. In-game at 240 fps (no Tracy client) → RSS flat for 5+ min. Was: ~0.2 MB/s growth.
3. Tracy GUI connected → zone, alloc, and frame-mark events populate from connection forward. Identical to pre-fix capture experience.

---

## Deviations from issue #30

The issue body (2026-04-05) listed investigation hypotheses written before the Tracy correlation was known: profile via MemoryTracker, audit per-frame Vulkan resources, check VMA, check render graph frame allocator reset, verify triple-buffered cleanup. None of those were the cause. The actual fix is one token in the build configuration. The lesson — when `MemoryTracker` reads balanced and RSS still grows, the bytes are by definition outside the tracker's domain (STL, third-party libs, *or Tracy's own buffers*); checking for build-system feedback loops is cheap and should come earlier.

---

## Follow-up (not in this release)

The investigation surfaced per-frame allocation hygiene in `ProjectLauncher` (`RelativeTimeString` rebuild, `fs::path.string()` per recent project, `std::getenv` per render call) and an `ImGui::ShowDemoWindow()` left enabled in `LuthienApp.cpp`. With `TRACY_ON_DEMAND` these cost zero memory when no client is connected, so they are perf, not correctness. Worth a separate `refactor/launcher-alloc-hygiene` issue if profiling later shows it matters.
