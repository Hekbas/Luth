# AGENTS.MD - Luth Engine Development Protocols
**Version:** 2.0 (Fiber-Native / Vulkan 1.3)
**Context:** High-Performance C++20 Game Engine
**Strict Constraint:** Zero OS-blocking on worker threads.

## 1. Architectural North Star
We are building a **Latency-Hiding, Fiber-Based Game Engine** inspired by Naughty Dog's engine architecture.
* **Logical Concurrency:** Use Fibers, not OS Threads. Logic is broken into small, dependency-aware Jobs.
* **Physical Execution:** $N$ Worker Threads pinned to cores (where $N = $ logical cores).
* **Rendering:** Vulkan 1.3 Dynamic Rendering. The GPU is fed by a Frame Graph compiler.
* **Synchronization:** Lock-Free internally. Timeline Semaphores externally.

### The "No-Fly" List (Strict Prohibitions)
1.  **NO** `std::mutex` or `std::condition_variable` inside Jobs. (Use SpinLock for <100 cycles, or `WaitForCounter` for long waits).
2.  **NO** `std::this_thread::yield()` or `sleep()`. If a job waits, it switches fibers, keeping the OS thread 100% busy with other work.
3.  **NO** `vkWaitForFences` on the CPU. We poll Timeline Semaphores.
4.  **NO** `VkRenderPass` / `VkFramebuffer`. Use `VkRenderingInfo` (Dynamic Rendering).

## 2. Core Systems Implementation Plan

### 2.1. The "Naughty Dog" Job System (Refactor `JobSystem.cpp`)
* **Problem:** Current implementation uses a global `std::mutex` (High Contention) and `std::this_thread::yield()` (OS Blocking).
* **Target:** Lock-Free Work Stealing with Fiber Context Switching.

**Data Structure: `Fiber`**
* Must contain `jmp_buf` / `ucontext` / Win32 Fiber Handle.
* **Crucial:** Must have `Fiber* nextWaiting` to act as an intrusive linked list node (avoids `std::list` allocations during waits).

**Data Structure: `AtomicCounter`**
* Replaces `std::atomic<int>`.
* Contains:
  * `std::atomic<uint32_t> value`
  * `std::atomic<Fiber*> waitingListHead` (Lock-free stack of fibers waiting for this counter).

**Algorithm: `WaitForCounter(Counter* c, uint32_t target)`**
1.  **Check:** `if (c->value <= target) return;`
2.  **Helping:** Try to pop a job from the local queue and execute it (help the system while waiting).
3.  **Suspend (If no help available):**
  * Add `CurrentFiber` to `c->waitingListHead` using CAS (Compare-And-Swap).
  * **Switch Context:** Call `SwitchFiber(SchedulerFiber)`.
  * The OS Thread does **NOT** sleep. It just loads a new stack.

**Algorithm: `WorkerThread`**
* Owns a Chase-Lev Deque (Lock-Free Work Stealing Deque).
* **Loop:**
  1.  Pop Local (LIFO - Hot Cache).
  2.  If empty, Steal from Random Victim (FIFO - Cold Cache).
  3.  If empty, Poll TimelineSemaphores (GPU check).
  4.  If empty, `_mm_pause()` (Brief spin).

### 2.2. The 3-Stage Frame Pipeline (New Implementation)
We decouple Game Logic from Rendering via deep pipelining. Data flows through a ring buffer of FrameData.

**Concept:** At any wall-clock time $T$, we are processing 3 frames simultaneously:
1.  **Frame N (Game Simulation):** CPU writing new transforms/gameplay state.
2.  **Frame N-1 (Render Record):** CPU reading N-1 state, generating Vulkan Commands.
3.  **Frame N-2 (GPU Execute):** GPU executing N-2 commands.

**Structure: `FrameContext` (Ring Buffer Size: 3)**
```cpp
struct FrameContext {
    // 1. Data Packet (Written by Game, Read-Only by Render)
    FrameParams params;             // Matrices, Lights, DrawLists
    LinearAllocator logicMemory;    // Per-frame allocations (cleared after GPU finishes)

    // 2. Synchronization
    AtomicCounter gameReady;        // Signaled when Game Logic finishes this frame
    uint64_t gpuTimelineValue;      // The value the GPU signals when done

    // 3. Render Resources
    LinearAllocator renderMemory;   // For temporary command arrays/barriers
    CommandAllocatorPool* cmdPool;  // Thread-local command pools for this frame
};
```

**The Engine Loop (Main Thread):**
```cpp
void EngineLoop() {
    uint64_t frameIdx = 0;
    while (Running) {
        FrameContext& frameN   = GetFrame(frameIdx);
        FrameContext& frameNm1 = GetFrame(frameIdx - 1);
        FrameContext& frameNm2 = GetFrame(frameIdx - 2);

        // 1. RECLAIM: Ensure Frame N-2 is fully done on GPU before overwriting N
        WaitForGPU(frameNm2.gpuTimelineValue); 
        frameN.ResetAllocators(); // Wipe memory for the new frame

        // 2. GAME JOB (Frame N): Kick off simulation
        JobSystem::Kick([&frameN] { RunGameSimulation(frameN); });

        // 3. RENDER JOB (Frame N-1): Record commands for the *previous* frame
        JobSystem::Kick([&frameNm1] { 
            WaitForCounter(frameNm1.gameReady); // Ensure N-1 Game data is ready
            RenderGraph::Execute(frameNm1);     // Massive parallel recording
        });

        // 4. SUBMIT (Frame N-1): 
        // Note: We submit N-1. N is still being simulated!
        SubmitToGPU(frameNm1); 

        frameIdx++;
    }
}
```

### 2.3. Vulkan & Frame Graph (Refactor `RenderingSystem.cpp`)
* **Dynamic Rendering:** All passes use `vkCmdBeginRendering`.
* **The Graph:**
  * **Setup Phase:** Systems declare `Read(Resource)` and `Write(Resource)`.
  * **Compile Phase:**
    * **Topological Sort** (Determine execution order).
    * **Barrier Injection:** Calculate `VkImageMemoryBarrier2` automatically based on usage transitions (e.g., `ColorAttachment` -> `ShaderReadOnly`).
    * **Aliasing:** Reuse memory for transient targets (e.g., G-Buffer and DoF buffer might share the same VRAM if their lifetimes don't overlap).
* **Thread Safety:**
  * Command Recording uses Thread-Local Command Pools.
  * **Constraint:** A Fiber cannot hold a `VkCommandBuffer` open across a `WaitForCounter`. (If a fiber yields, it might resume on a different thread, violating command pool threading rules).
  * **Solution:** Record "Chunks" (Secondary Buffers) or atomic "Passes" that complete without yielding.

## 3. Directory Structure Refinement
Align the codebase to this architecture:
```text
luth/
├── core/
│   ├── fibers/
│   │   ├── FiberContext.asm      // Assembly for context switching
│   │   ├── Scheduler.cpp         // Work-Stealing implementation
│   │   └── Counter.h             // AtomicCounter + Intrusive Wait List
│   ├── memory/
│   │   └── LinearAllocator.h     // Essential for FrameParams
│   └── FrameData.h               // Defines FrameContext & FrameParams ring buffer
├── renderer/
│   ├── backend/
│   │   ├── vulkan/
│   │   │   ├── TimelineSemaphore.h // Wrapper for vkGetSemaphoreCounterValue
│   │   │   └── CommandPool.h       // Thread-Local Pool wrapper
│   ├── graph/
│   │   ├── RenderGraph.cpp       // Compiler & Executor
│   │   ├── PassNode.h            // Concepts for render passes
│   │   └── ResourceBarrier.cpp   // Auto-barrier logic
```

## 4. Implementation Checklist for Agent
When generating code, verify against these items:

- [ ] **Does `WaitForCounter` block the OS thread?**
  * Yes: **REJECT.**
  * No (Switches Fiber): **APPROVE.**
- [ ] **Are we using global locks in the hot loop?**
  * Yes: **REJECT.** Use `std::atomic` and per-thread queues.
- [ ] **Is the Render Graph compiling automatically?**
  * Manual barriers are banned. The graph must inject them.
- [ ] **Is the Frame Pipeline latency correct?**
  * Render Job must process `FrameIndex - 1`.
  * Memory reclamation must happen at `FrameIndex - 2` (or 3).

**End of Protocols.**