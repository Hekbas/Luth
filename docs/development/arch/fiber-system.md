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
| V6 | **GPU Stall ↔ Allocator Reset Deadlock** — GPU stalls on N-2, allocator for Frame N can't be reset | Overflow allocator tier. Frame N uses overflow page from CPU `TaggedPageAllocator` and/or GPU `GPUTaggedPageAllocator` (Onion/Garlic both halves wired post-`gpu-tagged-heap`); reclaimed via the direct `IsFrameComplete` sweep in `VulkanBackend::AcquireImage` |

## Key Design Decisions

- **Custom x86_64 MASM context switch** (post-`custom-fibers` v2.11.0) — `make_fcontext` + `jump_fcontext` over `VirtualAlloc`-backed 2 MB stacks with a low-end guard page. Replaces Win32 `CreateFiberEx`/`SwitchToFiber` so ASan can track per-fiber stack bounds (Win32 fibers expose no bounds before the first switch). ASan `start_switch_fiber` / `finish_switch_fiber` hooks paired around the asm switch in the C++ wrapper (Folly pattern).
- **Per-fiber JobContext lookup via TIB ArbitraryUserPointer** (`gs:[0x28]`) — swapped per switch by the MASM save/restore of NT_TIB fields (StackBase / StackLimit / DeallocationStack / FiberData / ArbitraryUserPointer). Replaces Win32 FLS; access via `__readgsqword(0x28)` / `__writegsqword(0x28)` intrinsics.
- **`JobContext` fields**: `InlineDepth` (u32, V5), `IsRecording` (bool, V3).
- **Worker loop order**: Ready Fibers → Global High Queue (MPMC) → Local Deque (LIFO) → Steal (FIFO) → Poll GPU → `WaitOnAddress` with generation counter (V4).
- **`WaitForCounter` policy (V5)**: If `InlineDepth < 4`, execute inline with `InlineDepth++`. If ≥ 4, `AllocateFiber()` → `SwitchTo()`. The yield path loops back after wake to re-check the busy bit — the v2.10.0 UAF was caused by `return`ing past the wake before `DecrementCounter`'s trailing `fetch_sub(1)` cleared the bit; foundation-testing (v2.11.1) carries the regression test.
