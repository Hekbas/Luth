# Fiber System — Design & Vulnerability Mitigations

## Constraint
Zero OS-thread blocking in gameplay/render hot path.

## Vulnerability Mitigations

| # | Hazard | Mitigation |
|---|--------|------------|
| V1 | **Lock + Yield Inversion** — Yielding while holding a spin-lock causes convoy effects and cache destruction | Spin-locks only for < 100 cycle critical sections. Never yield under a lock. Long operations must be lock-free atomic states or dependent job chains |
| V2 | **Main Thread Starvation** — Main thread stealing heavy jobs blocks `glfwPollEvents()` | Main thread is isolated: OS poll → frame orchestration → present. Never enters worker steal loop |
| V3 | **VkCommandBuffer Thread Violation** — Implicit `WaitForCounter` migrates fiber to different OS thread, violating `VkCommandPool` affinity | RAII `RecordingScope` sets `IsRecording` in FLS. `Fiber::Yield()` asserts `IsRecording == false`. Hard crash in debug |
| V4 | **`WaitOnAddress` Lost Wakeup** — Worker calls `WaitOnAddress` after producer inserts work + wakes | Compare-and-wait pattern with generation counter. All queue insertions pair with `WakeByAddressSingle`. ABA-safe validation loop |
| V5 | **Sub-Job Context Switch Thrashing** — Forcing `SwitchToFiber` for trivial sub-jobs | Depth-limited inline execution. `InlineDepth` counter in `JobContext`, inline up to depth 4, then mandatory fiber switch |
| V6 | **GPU Stall ↔ Allocator Reset Deadlock** — GPU stalls on N-2, allocator for Frame N can't be reset | Overflow allocator tier. Frame N uses overflow `TaggedPageAllocator` pool; reclaimed when N-2 completes |

## Key Design Decisions

- **FLS via Win32 API** — `FlsAlloc`/`FlsSetValue`/`FlsGetValue` instead of `thread_local`. Each fiber carries its own `JobContext*`.
- **`JobContext` fields**: `InlineDepth` (u32, V5), `IsRecording` (bool, V3).
- **Worker loop order**: Ready Fibers → Global High Queue (MPMC) → Local Deque (LIFO) → Steal (FIFO) → Poll GPU → `WaitOnAddress` with generation counter (V4).
- **`WaitForCounter` policy (V5)**: If `InlineDepth < 4`, execute inline with `InlineDepth++`. If ≥ 4, `AllocateFiber()` → `SwitchTo()`.
