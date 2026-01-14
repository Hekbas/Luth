# Luth Engine Architecture & Development Plan

**Status:** Active
**Philosophy:** Data-Oriented, Jobified (N:M Fibers), Vulkan-First (Bindless).
**Goal:** AAA-grade architectural rigor; Zero "Main Thread" blocking.

---

## 0. System Hierarchy & Data Flow

### Architecture Tree
```text
[Engine Root]
 ├── [Core]
 │    ├── JobSystem (Fibers) ................... N:M Task Scheduler (Lock-Free Chase-Lev Deque)
 │    │    ├── Fiber Context ................... Stack (VirtualAlloc), FLS (JobContext), Guard Pages
 │    │    └── Synchronization ................. Atomic Counters, Adaptive Mutexes, Poller Jobs
 │    ├── EventBus ............................. Double-Buffered / Deferred Dispatch
 │    └── Memory ............................... Allocators
 │         ├── TaggedPageAllocator ............. 2MB Pages with Per-Thread Cache (Frame-Lifetime)
 │         └── PoolAllocator ................... Fixed-size components/fibers
 │
 ├── [Asset Pipeline]
 │    ├── AssetDatabase ........................ Metadata registry (Assets/ -> UUID)
 │    ├── Library .............................. Binary artifacts cache (Disk)
 │    └── AssetManager ......................... Runtime lifecycle
 │         ├── Importers ....................... Convert Source -> Artifact
 │         └── UploadContext ................... Async Transfer Queue (Staging Ring Buffer)
 │
 ├── [Scene / ECS]
 │    ├── Registry (EnTT) ...................... Entity/Component storage
 │    └── Systems .............................. Logic execution
 │         ├── TransformSystem ................. Parallel Level-Order Hierarchy
 │         ├── CameraSystem .................... View/Projection calculation
 │         └── RenderingSystem ................. Graph Compiler & Cull Jobs
 │
 └── [Rendering (Vulkan 1.3)]
      ├── RenderGraph .......................... DAG Construction -> Barrier Injection -> Aliasing
      ├── ResourceCache ........................ Transient resource reuse
      ├── Bindless Descriptors ................. Global "Mega-Texture" Array (Set 0)
      └── Backend .............................. Device abstraction
           ├── Dynamic Rendering ............... No RenderPass/Framebuffer objects
           ├── FrameData ....................... Triple Buffered (Semaphores, DeletionQueue)
           │    └── CommandAllocatorPool ....... Thread-Safe Rental Shop (Context-Carried)
           └── Synchronization ................. Timeline Semaphores, Poller Jobs
```

### Execution Flow (Pipelined Frames)
**Concept:** `FrameParams` flow through three independent stages. At any time `T`:

1.  **Frame N (Game CPU)**:
    * **Activity:** Main Thread + Workers run Game Logic (Physics, AI).
    * **Output:** Writes to `FrameParams[N]`.
2.  **Frame N-1 (Render CPU)**:
    * **Activity:** Workers record Vulkan Commands (RenderGraph).
    * **Input:** Reads `FrameParams[N-1]` (Immutable).
    * **Output:** Produces `VkCommandBuffer`s.
3.  **Frame N-2 (GPU Exec)**:
    * **Activity:** GPU executes the commands submitted for N-1.
    * **Input:** Reads buffers/textures referenced by N-2.
    * **Recycle:** When `PollerJob` sees N-2 complete, it frees `Tag(N-2)` on the Tagged Heap.

**Constraint:** The Main Thread submits the command buffers for **N-1** at the end of the frame, then immediately starts Game Logic for **N+1**.

---

## 1. Core Architecture

### A. The Fiber System (Job System)
**Role:** Abstracts CPU cores into a pool of workers executing Fibers.
**Philosophy:** No blocking allowed. If a dependency is not met, the Fiber Yields.

* **Primitives:**
    * `Fiber`: Lightweight execution context (VirtualAlloc stack + Guard Page).
    * `JobContext`: The "Fiber Local Storage" (FLS). Contains pointer to current `TaggedAllocator` block and `CommandAllocator`.
    * `Adaptive Mutex`: Replaces standard Spinlock. Spins briefly (~2000 cycles) then calls `Fiber::Yield()` to avoid priority inversion or deadlocks.
* **Scheduler:**
    * **Priorities:** Three queues (High, Normal, Low).
    * **Policy:** "Game Logic" (High) always preempts "Asset Loading" (Low).
* **Execution Model:**
    * **Wait Strategy:** `WaitForCounter` switches execution to a new Fiber.
    * **Poller Job:** A specific job type that loops on `vkGetSemaphoreCounterValue`. If target not reached, it yields (re-queues itself) to allow other CPU work.
* **Safety Constraints:**
    * **NO `thread_local`**: Standard TLS is forbidden (unsafe due to migration). Use `JobContext`.
    * **Float Safety**: Respect ABI callee-saved registers (XMM6-XMM15) or enforce strict no-float zones across yields.

### B. Memory Management
**Role:** Lock-free, cache-friendly allocation for high-frequency data.

* **Tagged Page Allocator (Replaces Linear Allocator):**
    * **Structure:** Pool of 2MB Memory Blocks (Large Pages).
    * **Per-Thread Cache:** Each Worker Thread holds a pointer to its own active 2MB block to avoid mutex contention during allocation.
    * **Migration Handling:** If a Fiber yields on Thread A and resumes on Thread B, it simply allocates from Thread B's active block. The allocation is stamped with the current **Tag** (e.g., `Frame_N`).
    * **Lifecycle:** `FreeTag(TagID)` frees all blocks associated with a specific frame instantly. No individual destructors.
* **Staging Buffer (Ring Buffer):**
    * Persistent mapped buffer. Tracks "Fence Values" to know when regions are safe to overwrite.

### C. Event System
**Role:** Decouple systems without introducing blocking locks.
* **Architecture:** Double-Buffered Message Queue.
* **Thread Safety:**
    * **Producers:** Any Fiber can push an event (Atomic Index increment on a Ring Buffer).
    * **Consumers:** Events are dispatched only at specific sync points (e.g., start of frame) on the Main Thread/Fiber.
* **Immediate Dispatch:** Allowed *only* for thread-local logic. Cross-thread immediate dispatch is forbidden (risk of deadlock).

### D. Debugging & Profiling
**Role:** Observability in a system where "Call Stacks" are unreliable.
* **Tracy Integration:**
    * **Fiber Aware:** Use `TracyFiberEnter` / `TracyFiberLeave` to visualize jumping execution flow.
    * **Lock Contention:** Profile spin-lock wait times to detect over-subscription.
* **Stuck Job Detector (Watchdog):**
    * A dedicated OS thread monitors active job timestamps.
    * **Trigger:** If a job runs > 100ms, it pauses execution and dumps the specific Fiber Stack.
* **Console:** CVars for runtime tuning of job priorities and memory pool sizes.

---

## 2. Asset Pipeline

**Role:** Asynchronous loading, processing, and management of resources.

### A. Asset Database & Manager
* **Runtime:** Loads exclusively from binary artifacts in `Library/`.
* **Async Loading (Dedicated Thread):**
    * **Constraint:** Fiber Workers are banned from touching Disk I/O.
    * **Architecture:** A dedicated OS thread (The "I/O Thread") handles blocking file reads.
    * **Flow:** Fiber requests load -> I/O Thread reads bytes -> I/O Thread spawns "Parse Job" (Fiber) -> Upload Context.

### B. Importers
* **ModelImporter:** Standardizes coords (Y-Up), MikkTSpace tangents, MeshOptimizer.
* **TextureImporter:** BC7/BC5 compression with Mipmaps.

---

## 3. Rendering Architecture (Vulkan 1.3)

**Role:** Explicit, high-performance, bindless.

### A. The Render Graph (Frame Graph)
**Role:** Automates synchronization and transient memory aliasing.
* **Structure:** DAG of `RenderPass` (Logic) + `Resource` (Data).
* **Compilation:**
    * Topological Sort -> Barrier Injection (`vkCmdPipelineBarrier2`).
    * **Memory Aliasing:** Allocates non-overlapping transient textures (e.g., Depth, G-Buffer) in the same physical `DeviceMemory` block to save VRAM.
* **Execution:**
    * Graph nodes are dispatched as **Parallel Jobs**.
    * **Command Recording:** Workers record into Secondary Command Buffers.

### B. Shader & Material System (Bindless)
**Role:** Decoupled data binding.
* **Global Heap:**
    * Single `DescriptorSet` (Set 0) binding global arrays of Textures/Buffers (`binding = 10`, `VK_EXT_descriptor_indexing`).
    * **Update Strategy:** Updates handled via `StagingBuffer` and aliased to the set.
* **Material Data:**
    * Materials are POD structs containing `uint32_t textureIndex`.
    * Data is uploaded to a massive `StorageBuffer` (MaterialBuffer).
    * **Push Constants:** Shader receives `materialID` and fetches data manually.
* **Benefit:** No `vkCmdBindDescriptorSets` per object. Massive CPU perf gain.

### C. Command Management (Job-Local Allocation)
**Goal:** Thread-safe, lock-free command recording compatible with Fiber migration.

* **Structure:** `CommandAllocator` (Wraps `VkCommandPool`).
    * **Internal:** Maintains a `std::vector<VkCommandBuffer>` cache.
    * **Method `GetBuffer()`:** Allocates from cache or Vulkan. No Mutex (Owned by Job).
    * **Method `Reset()`:** Resets the underlying `VkCommandPool`.
* **Global Pool (`CommandAllocatorPool`):**
    * **Role:** Thread-safe collection (`ConcurrentQueue`).
    * **Acquire:** Job grabs an allocator. Mutex/Atomic used only here.
    * **Release:** Allocator returned ONLY after GPU completes the frame.
* **Integration:**
    * `JobContext` holds `CommandAllocator* cmdAllocator`.
    * **Lazy Initialization:** Job acquires allocator only if it needs to record.
* **Lifecycle (Frame Loop):**
    1.  **Job:** Acquires Allocator -> Records Buffers -> Pushes Allocator to `FrameData::PendingList`.
    2.  **Submit:** Main Thread submits all buffers in `PendingList`.
    3.  **Recycle:** `PollerJob` waits for Frame N-2. Calls `Reset()` on all allocators in N-2 list and pushes them back to `GlobalPool`.

### D. Backend & Synchronization
* **Dynamic Rendering:** `vkCmdBeginRendering` (No `VkRenderPass` objects).
* **Synchronization:**
    * **Timeline Semaphores:** Unifies Compute/Graphics/Transfer sync. Replaces `vkWaitForFences` entirely.
    * **Poller Job:** "Wait-Free" GPU synchronization on CPU.
* **Frame Data:**
    * Triple Buffered `FrameContext` (CommandAllocators, DeletionQueue).

---

## 4. Scene System (ECS)

**Role:** Game logic and state management (EnTT).
* **Systems:**
    * `TransformSystem`: Parallel Level-Order Traversal.
    * `RenderingSystem`: Culling and Draw Packet generation.
    * `SceneSerializer`: YAML Save/Load.

---

## 5. Editor Architecture

**Role:** Tools for creation and debugging (Dear ImGui).
* **Panels:** Scene, Hierarchy, Inspector, Project, Console, Profiler.
* **Scene View:** ImGuizmo integration, Framebuffer picking.

---

## 6. Implementation Roadmap

### Phase 5: Core Systems (Memory & Concurrency)
**Goal:** Establish the memory and execution foundation. No Vulkan implementation in this phase.

**5.1 Memory Architecture**
- [x] **Tagged Page Allocator:**
    - [x] Implement `TaggedPageAllocator` (Pool of 2MB `VirtualAlloc` pages).
    - [x] Implement `FreeTag(uint32_t tag)` for bulk reclamation.
    - [x] Implement `ThreadCache` (Index-based access via `JobContext`) to minimize global lock contention.
- [x] **Frame Architecture:**
    - [x] Define `FrameParams` struct (Inputs, Time, Matrices, Viewport).
    - [x] Implement Triple Buffering for `FrameParams` (Game N, Render N-1, GPU N-2).
- [x] **Job Context:**
    - [x] Define `struct JobContext`.
    - [x] **Fields:** `TaggedAllocator*`, `FrameParams*`, `ThreadIndex`, `FiberID`.
    - [x] **Rule:** All Jobs must accept `JobContext&` as their primary argument.

**5.2 Fiber Runtime**
- [x] **Safety Audit:**
    - [x] Remove all `thread_local` variables. Replace with `JobContext` lookups.
    - [x] Verify `Guard Page` implementation (protects against stack overflow).
- [x] **Synchronization Primitives:**
    - [x] Implement `AdaptiveMutex` (Spin ~2000 cycles -> `Fiber::Yield()`).
    - [x] Implement `AtomicCounter` (Wait adds fiber to "Wait List", resume moves to "Ready List").
- [x] **Scheduler Upgrade:**
    - [x] Implement **Priority Queues** (High, Normal, Low).
    - [x] **Policy:** High Priority (Game Logic) preempts Low Priority (Asset Loading).

**5.3 Engine Loop**
- [x] **Main Loop Refactor:**
    - [x] Implement explicit pipeline staging: `KickGame(N)` -> `SubmitRender(N-1)` -> `RecycleGPU(N-2)`.
- [x] **I/O Subsystem:**
    - [x] Spawn dedicated `IO_Thread` (OS Thread).
    - [x] Implement `FileRequestQueue` (Lock-free ring buffer).
    - [x] **Constraint:** Blocking I/O (fread/fstream) forbidden in Fiber Workers.

---

### Phase 6: Vulkan Backend
**Goal:** Lock-free, explicit command generation utilizing the core memory model.

**6.1 Synchronization**
- [x] **Timeline Semaphores:**
    - [x] Create `TimelineSemaphore` wrapper.
    - [x] **Rule:** All Queue submits must signal a Timeline value (Frame Index).
- [x] **The Poller Job:**
    - [x] Implement `VulkanWaitJob`.
    - [x] **Logic:** `vkGetSemaphoreCounterValue` -> If < Target, Yield; Else, execute callback.
    - [x] **Cleanup:** Remove all `vkWaitForFences` calls.

**6.2 Command Buffer Management**
- [ ] **Command Allocator:**
    - [ ] Create `struct CommandAllocator` (owns `VkCommandPool` + `vector<VkCommandBuffer>`).
    - [ ] Implement `GetBuffer()` (Lock-free, internal cache).
    - [ ] Implement `Reset()` (Resets underlying pool).
- [ ] **Global Pool:**
    - [ ] Create `CommandAllocatorPool` (Thread-safe `ConcurrentQueue`).
    - [ ] **Acquire:** Pop allocator from queue.
    - [ ] **Release:** Push allocator back to queue.
- [ ] **Integration:**
    - [ ] Add `CommandAllocator*` to `JobContext`.
    - [ ] Implement "Lazy Acquire" in Render Jobs.
    - [ ] Implement "Frame End Release" (Recycle only when `Poller` confirms GPU done).

**6.3 Render Graph & Execution**
- [ ] **Render Graph Compiler:**
    - [ ] Implement DAG topological sort.
    - [ ] Implement `BarrierBuilder` (Inject `vkCmdPipelineBarrier2`).
- [ ] **Parallel Recording:**
    - [ ] Refactor `RenderGraph::Execute`:
        - [ ] Split Pass into N tasks (Secondary Buffers).
        - [ ] Dispatch Jobs (concurrent with Game N).
        - [ ] Coalesce Secondary Buffers into Primary for submission.

---

### Phase 7: Data & Bindless Pipeline
**Goal:** High-throughput resource streaming.

**7.1 Bindless Resources**
- [ ] **Global Descriptor Heap:**
    - [ ] Enable `VK_EXT_descriptor_indexing`.
    - [ ] Create Layout: `binding=10, uniform texture2D globalTextures[]`.
- [ ] **Material System:**
    - [ ] Refactor Materials to use `uint32_t textureID` instead of `VkDescriptorSet`.
    - [ ] Upload Material Data to a global `SSBO`.

**7.2 Asset Streaming**
- [ ] **Upload Context:**
    - [ ] Create dedicated `TransferQueue`.
    - [ ] Implement `StagingRingBuffer` (Persistent mapped memory).
- [ ] **Async Flow:**
    - [ ] `IO_Thread` reads binary -> Spawns `DecompressJob` -> Allocates Staging -> Records Copy -> Signals Timeline.

---

### Phase 8: Rendering Features
**Goal:** Graphical fidelity.

- [ ] **PBR Implementation:**
    - [ ] Load .HDR Environment Maps.
    - [ ] Implement IBL (Irradiance/Prefilter Compute Shaders).
- [ ] **Shadows:**
    - [ ] Cascaded Shadow Maps (CSM).
    - [ ] Parallel Shadow Render Pass.
- [ ] **Post-Processing:**
    - [ ] Tone Mapping (ACES).
    - [ ] Bloom (Compute Shader).

---

### Phase 9: Gameplay & Tooling
**Goal:** Engine usability.

- [ ] **Scene Serialization:** YAML Save/Load via EnTT.
- [ ] **Physics:** Jolt Physics Integration (Jobified via `JobSystem`).
- [ ] **Scripting:** C# Mono Integration (Game Logic stage).
- [ ] **Editor:** ImGui Docking, Gizmos, Asset Browser.