# Phase 1: Job System Rewrite ✅ (2026-03-07)

**Goal:** Correct, fiber-safe scheduler with zero OS blocking.

### New Files
| File | Purpose |
|---|---|
| [SpinLock.h](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/core/SpinLock.h) | Pure spin-lock (V1). Test-then-TAS, `_mm_pause`, RAII guard |
| [MPMCQueue.h](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/core/MPMCQueue.h) | Vyukov MPMC (V4). `WakeByAddressSingle` on push |
| [WorkStealingDeque.h](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/core/WorkStealingDeque.h) | Chase-Lev deque. LIFO push/pop, FIFO steal |

### Rewritten Files
| File | Changes |
|---|---|
| [JobSystem.h](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/core/JobSystem.h) | Priority enum, JobContext, RecordingScope |
| [JobSystem.cpp](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/core/JobSystem.cpp) | FLS, MPMC, Chase-Lev, isolated main (V2), inline exec (V5) |
| [Fiber.h](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/core/Fiber.h) | `SwitchTo` asserts `!IsRecording` (V3) |
| [IOThread.cpp](file:///c:/Users/Hekbas/CITM/5_TFG/Luth/luth/source/luth/core/IOThread.cpp) | Pre-allocated ring of 64 callback slots |

### Key Design Decisions
- **FLS via Win32 API** — `FlsAlloc`/`FlsSetValue`/`FlsGetValue` instead of `thread_local`. Each fiber carries its own `JobContext*`.
- **`JobContext` extended fields**: `InlineDepth` (u32, for V5 depth-limited inline exec), `IsRecording` (bool, for V3 yield assertion).
- **Worker loop**: Check Ready Fibers → Global High Queue (MPMC) → Local Deque (LIFO) → Steal (FIFO) → Poll GPU → `WaitOnAddress` with generation counter (V4).
- **Main thread is ISOLATED (V2)**: OS message pump, frame orchestration, present. Never enters worker steal loop.
- **`WaitForCounter` sub-job policy (V5)**: If `InlineDepth < 4`, execute inline. If ≥ 4, fiber switch.

### Commit
```
feat(core): rewrite job system + lock-free primitives
```
