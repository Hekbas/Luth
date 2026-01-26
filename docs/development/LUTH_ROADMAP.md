# ROADMAP.MD - Luth Engine Implementation Path
**Objective:** Transform Luth into a high-performance, fiber-based, pipelined game engine.
**Status:** DRAFT -> ACTIVE

 ---

## Phase 1: The "Naughty Dog" Fiber Kernel
*Goal: Establish the non-blocking execution environment. The engine must be able to spawn 100,000 empty jobs without stalling the OS threads.*

### 1.1. Low-Level Context Switching
- [ ] **Implementation:** Create struct `FiberContext`.
    * **Windows:** Wrap `CreateFiberEx` / `SwitchToFiber`.
    * **Linux:** Implement assembly context switch (save `rbx`, `rbp`, `r12-r15`, `rip`, `rsp`).
- [ ] **Stack Management:** Implement `FiberPool`.
    * Allocate a large virtual memory block (e.g., 64MB).
    * Commit pages on demand (don't commit all 64MB at startup).
    * **Constraint:** Use guards (protected pages) between stacks to catch overflows.

### 1.2. Atomic Counter & Wait List
- [ ] **Data Structure:** Create struct `AtomicCounter`.
    * `std::atomic<uint32_t> value`
    * `std::atomic<Fiber*> waitingListHead` (Lock-Free Stack).
- [ ] **Mechanism:** Implement `AddDependency(AtomicCounter* c, Fiber* f)`.
    * Use CAS (Compare-And-Swap) to push the fiber onto the `waitingListHead` of the counter.
- [ ] **Verification:** Write a unit test where Fiber A waits on Fiber B. Ensure Fiber A is not scheduled until Fiber B finishes.

### 1.3. Scheduler (Hybrid Queues)
- [ ] **Task:** Implement **Global High-Priority Queue** (Lock-Free MPMC).
- [ ] **Task:** Implement **Local Work-Stealing Deque** (Chase-Lev) for Normal priority.
- [ ] **Task:** Implement **WorkerThread** loop:
  1. Check Global High.
  2. Check Local.
  3. Steal.

### 1.4. Wait Strategy (Safe Helping)
- [ ] **Task:** Implement `while(counter > 0) { if (TryGetLocalJob(out job)) { RunJobOnNewFiber(job); } else { Suspend(); } }`.
  * Logic: `std::vector<Job> localQueue` (Circular buffer).
  * Crucial: Verify `RunJobOnNewFiber` grabs a new fiber from the pool and context switches. **Do not call inline**.

 ---

## Phase 2: The Data Pipeline (Triple Buffer)
*Goal: Implement the memory infrastructure required to run Game, Render, and GPU logic in parallel without locks.*

### 2.1. Frame Architecture
- [ ] **Struct:** Define `FrameContext`.
```cpp
struct FrameContext {
    uint64_t frameId;
    LinearAllocator linearMemory; // Reset every 3 frames
    //...
};
```
- [ ] **Storage:** Create `FrameContext frames[3]` (Triple Buffering).

### 2.2. Linear Allocator
- [ ] **Implementation:** Create `LinearAllocator` (Reset-only bump allocator).
    * **Pointer:** `std::atomic<byte*> currentPtr`.
    * **Block:** Pre-allocated 128MB block per frame.
    * **Constraint:** No `delete` or `free`. We only call `Reset()` when the frame index loops back.

### 2.3. Engine Loop Refactor
- [ ] **Main Loop:** Rewrite `App.cpp` loop.
    * **Old:** `Update()` -> `Render()` -> `Present()`.
    * **New:**
        1.  `Wait(Frame[N-2].gpuFinished)`
        2.  `Reset(Frame[N])`
        3.  `KickJob(GameUpdate, Frame[N])`
        4.  `KickJob(RenderRecord, Frame[N-1])` (Dependent on `Frame[N-1].gameReady`)
        5.  `Submit(Frame[N-1])`

 ---

## Phase 3: Vulkan 1.3 Backend (Thread-Safe Recording)
*Goal: Record command buffers in parallel fibers without driver crashes.*

### 3.1. Thread-Local Contexts
- [ ] **RHI:** Create `RenderThreadContext` (allocated per Worker Thread, not per Fiber).
    * `VkCommandPool` (One per thread).
    * `DescriptorAllocator` (One per thread).
- [ ] **Access:** Implement `GetRenderContext()` that returns the context of the current worker thread executing the fiber.

### 3.2. Dynamic Rendering Wrapper
- [ ] **API:** Abstraction for `vkCmdBeginRendering`.
    * **Input:** `std::span<TextureHandle> attachments`.
    * **Logic:** Populate `VkRenderingInfo`. No `VkRenderPass` objects allowed.

### 3.3. Chunked Recording
- [ ] **Logic:** Implement `RenderPassJob`.
    * **Start:** `vkAllocateCommandBuffers` (Secondary or Primary Chunk).
    * **Record:** Draw calls.
    * **End:** `vkEndCommandBuffer`.
    * **Output:** Push the completed `VkCommandBuffer` handle to a thread-safe list in `FrameContext`.

 ---

## Phase 4: Synchronization & The Render Graph
*Goal: Automatic barrier generation and async compute.*

### 4.1. The Graph Data Model
- [ ] **Structs:**
    * `RGResource`: Logical handle (Texture/Buffer).
    * `RGPass`: Contains read list, write list, and execution lambda.
- [ ] **Builder:** `RGBuilder::Read(handle)`, `RGBuilder::Write(handle)`.

### 4.2. The Compiler (Topological Sort)
- [ ] **Algorithm:**
    1.  Filter out passes with no side effects (culling).
    2.  Sort passes based on dependencies.
    3.  Calculate resource lifetimes (first usage, last usage).

### 4.3. Barrier Solver
- [ ] **Logic:**
    * Iterate the sorted pass list.
    * Track current state of every resource (e.g., `Undefined`).
    * If Pass A writes (`ColorTarget`) and Pass B reads (`ShaderResource`):
        * Inject `VkImageMemoryBarrier2` between A and B.
        * Transition: `COLOR_ATTACHMENT_OPTIMAL` -> `SHADER_READ_ONLY_OPTIMAL`.

### 4.4. Timeline Semaphore Integration
- [ ] **Sync:**
    * Create global TimelineSemaphore `gpuTimeline`.
    * **On Submit:** `signal(gpuTimeline, frameId)`.
    * **On CPU Wait:** `WaitForGPU(frameId)` -> Fiber yields if `gpuTimeline < frameId`.

 ---

## Phase 5: Verification & Stress Testing

### 5.1. The "Million Job" Test
- [ ] **Test:** Spawn 1,000,000 jobs that simply increment an atomic integer.
- [ ] **Metric:** Verify CPU usage is 100% on all cores and overhead is < 5%.

### 5.2. The "Flicker" Test
- [ ] **Test:** Render a scene with high-frequency updates (every frame).
- [ ] **Verify:** Ensure no tearing or "time travel" (rendering frame N using data from frame N-1 or N+1 due to incorrect ring buffer indexing).

### 5.3. Validator Cleanliness
- [ ] **Check:** Run with Vulkan Validation Layers + Synchronization Validation (`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`). Ensure 0 errors.