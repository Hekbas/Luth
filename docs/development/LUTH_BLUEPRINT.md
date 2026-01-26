# AGENTS.MD - Luth Engine Development Protocols
**Version:** 3.0 (Fiber-Native / Vulkan 1.3)
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
4.  **NO Inline Execution of Jobs:** When "helping" during a wait, you must **ALWAYS** switch to a fresh fiber. Never run a job as a function call on the current stack.

## 2. Core Systems Implementation Plan

### 2.1. The Hybrid Job System (Priority + Work Stealing)
* **Problem:** Pure Work Stealing ignores priority. Pure Global Queues contend on high-core-count PCs.
* **Solution:** Hybrid Priority Model.

**Queues:**
* **Global High-Priority Queue (MPMC):** For critical path tasks (Audio, Physics, Render Recording).
* **Local Normal-Priority Deques (Chase-Lev):** For general gameplay/entity logic. One per thread.

**Worker Loop Algorithm:**
```cpp
while (Running) {
Job job;
// 1. Strict Priority: Always check Global High Queue first
if (GlobalHighQueue.TryPop(job)) {
RunJob(job);
continue;
}

    // 2. Local Depth-First: Check Local Deque (LIFO) for cache locality
    if (LocalDeque.Pop(job)) {
        RunJob(job);
        continue;
    }

    // 3. Load Balancing: Steal from others (FIFO)
    if (Steal(job)) {
        RunJob(job);
        continue;
    }

    // 4. GPU Polling (If idle)
    PollTimelineSemaphores();
}
```

**Data Structure: `Fiber`**
* Must contain `jmp_buf` / `ucontext` / Win32 Fiber Handle.
* **Crucial:** Must have `Fiber* nextWaiting` to act as an intrusive linked list node (avoids `std::list` allocations during waits).

**Data Structure: `AtomicCounter`**
* Replaces `std::atomic<int>`.
* Contains:
  * `std::atomic<uint32_t> value`
  * `std::atomic<Fiber*> waitingListHead` (Lock-free stack of fibers waiting for this counter).

### 2.2. The "Help-First" Wait Strategy (Stack Safe)
* **Context:** When `WaitForCounter(C)` is called, the fiber cannot proceed until `C == 0`.
* **Strategy:** Instead of yielding immediately (ND style), we try to "help" by running local sub-jobs to keep the cache hot.

**Strict Safety Rule:** When helping, you must **Acquire a Fresh Fiber** and **SwitchTo** it.
* **Violation:** `job.function()` (Inline Call) -> **REJECT.** (Causes Stack Overflow on deep graphs).
* **Compliance:** `fiber = Pool.Get(); Setup(fiber, job); SwitchTo(fiber);` -> **APPROVE.**

### 2.3. The 3-Stage Frame Pipeline
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

## 3. Vulkan 1.3 Backend

### 3.1. API Constraints
* **Dynamic Rendering:** Use `vkCmdBeginRendering`.
* **Bindless Descriptors:** Use `VK_EXT_descriptor_indexing`. Bind one global descriptor heap (sampled images, storage buffers) at slot 0. Access resources via indices in Push Constants.

### 3.2. Thread Safety & Recording
* **Thread-Local Pools:** `VkCommandPool` is Thread-Local. Never share a pool across threads.
* **Chunked Recording:** A fiber must **NOT** yield while holding an open `VkCommandBuffer`.
  * **Strategy:** Record a "Chunk" (Secondary Buffer), close it, push it to a thread-safe list in `FrameContext`, **THEN** yield.
  * **Resumption:** If the fiber resumes on a different thread, it allocates a new buffer from that new thread's pool.

### 3.3. Synchronization
* **Timeline Semaphores:** Use a global `FrameTimeline` semaphore.
* **Polling:** The Scheduler loop runs a `PollGPU()` function periodically to check if Frame N-2 is complete.

## 4. Directory Structure Refinement
Align the codebase to this architecture:
```text
luth/
├── core/
│   ├── fibers/
│   │   ├── FiberContext.asm        // Assembly for context switching
│   │   ├── Scheduler.cpp           // Work-Stealing implementation
│   │   ├── Counter.h               // AtomicCounter + Intrusive Wait List
│   │   └── WorkStealingQueue.h     // Chase-Lev Deque
│   ├── memory/
│   │   └── LinearAllocator.h       // Per-frame CPU memory
│   └── FrameData.h                 // Defines FrameContext & FrameParams ring buffer
├── renderer/
│   ├── backend/
│   │   ├── vulkan/
│   │   │   ├── TimelineSemaphore.h // Wrapper for vkGetSemaphoreCounterValue
│   │   │   ├── DescriptorHeap.h    // Bindless management
│   │   │   └── CommandPool.h       // Thread-Local Pool wrapper
│   ├── RenderGraph/
│   │   ├── Compiler.cpp            // Topological sort & barrier injection
│   │   ├── ResourceAllocator.cpp   // Transient aliasing logic
│   │   └── Executor.cpp            // Spawns Jobs from compiled graph
```
## 5. Glossary
* **Yielding:** Saving current registers/stack, pushing fiber to a wait list, and loading a new fiber.
* **Helping:** Executing a pending job from the local queue while waiting for a counter, to avoid a full context switch overhead if possible (but **ALWAYS** switching stacks).
* **Transient Resource:** A texture/buffer that exists only for the duration of one frame. Backed by aliased memory.
* **Barrier Batching:** Merging multiple image transitions into a single `vkCmdPipelineBarrier2` call.

## 6. Implementation Checklist for Agent
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