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
 │    │    └── Synchronization ................. Atomic Counters, Spinlocks (with Yield), Poller Jobs
 │    ├── EventBus ............................. Double-Buffered / Deferred Dispatch
 │    └── Memory ............................... Allocators
 │         ├── FramePacketAllocator ............ Linear (Reset per frame). No destructors.
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
           ├── FrameData ....................... Triple Buffered (CmdPools per Thread, Semaphores)
           └── Synchronization ................. Timeline Semaphores, Poller Jobs
```

### Execution Flow (Per Frame)
1.  **Input**: Poll GLFW -> Dispatch Events -> Update Input State.
2.  **Asset Sync**: Process `UploadContext` (Copy Staging -> Device Local) via Transfer Queue.
    * [cite_start]*Constraint:* Must happen before rendering to ensure resources are valid[cite: 131, 132].
3.  **Logic Update (Jobified)**:
    * **Kick Jobs**: Asset Loading, Physics, Transform Updates, Culling.
    * [cite_start]**Wait Strategy**: Main Thread yields to Worker Pool; never sleeps[cite: 22].
4.  [cite_start]**Render Prep**: `RenderGraph` compiles passes and injects `VkImageMemoryBarrier2`[cite: 106, 110].
5.  **Render Execute**:
    * [cite_start]**Parallel Record**: Workers record into Secondary Command Buffers[cite: 76].
    * **Submit**: Main Thread submits to Graphics Queue.
    * **Present**: Standard Swapchain presentation.
    * **Constraint**: The CPU moves to Frame N+1 immediately. [cite_start]It checks Frame N's GPU completion via a **Poller Job** only when resource recycling is needed[cite: 88, 92].

---

## 1. Core Architecture

### A. The Fiber System (Job System)
**Role:** Abstracts CPU cores into a pool of workers executing Fibers.
**Philosophy:** No blocking allowed. If a dependency is not met, the Fiber Yields.

* **Primitives:**
    * [cite_start]`Fiber`: Lightweight execution context (VirtualAlloc stack + Guard Page)[cite: 46, 51].
    * `JobContext`: The "Fiber Local Storage" (FLS). [cite_start]Contains pointer to current `FrameAllocator` and `CommandAllocator`[cite: 58].
    * `Counter`: Atomic variable for dependency tracking.
* **Execution Model:**
    * [cite_start]**Wait Strategy**: `WaitForCounter` switches execution to a new Fiber[cite: 25].
    * **Poller Job**: A specific job type that loops on `vkGetSemaphoreCounterValue`. [cite_start]If target not reached, it yields (re-queues itself) to allow other CPU work[cite: 92, 178].
* **Safety Constraints:**
    * **NO `thread_local`**: Standard TLS is forbidden (unsafe due to migration). [cite_start]Use `JobContext`[cite: 33, 34].
    * [cite_start]**Float Safety**: Respect ABI callee-saved registers (XMM6-XMM15) or enforce strict no-float zones across yields[cite: 189].

### B. Memory Management
* **Frame Packet Allocator (Linear):**
    * Allocates per-frame transient data (RenderGraph nodes, Draw packets).
    * **Rule**: POD types only. [cite_start]Destructors are **not** run[cite: 123, 128].
* **Staging Buffer (Ring Buffer):**
    * Persistent mapped buffer. [cite_start]Tracks "Fence Values" to know when regions are safe to overwrite[cite: 131].

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
    * [cite_start]**Fiber Aware:** Use `TracyFiberEnter` / `TracyFiberLeave` to visualize jumping execution flow[cite: 236].
    * **Lock Contention:** Profile spin-lock wait times to detect over-subscription.
* **Stuck Job Detector (Watchdog):**
    * A dedicated OS thread monitors active job timestamps.
    * [cite_start]**Trigger:** If a job runs > 100ms, it pauses execution and dumps the specific Fiber Stack[cite: 231, 233].
* **Console:** CVars for runtime tuning of job priorities and memory pool sizes.

---

## 2. Asset Pipeline

**Role:** Asynchronous loading, processing, and management of resources.

### A. Asset Database & Manager
* **Runtime:** Loads exclusively from binary artifacts in `Library/`.
* **Async Loading:**
    * Spawns **Load Job** (Disk I/O -> Staging).
    * [cite_start]Pushes to `UploadContext` (Transfer Queue)[cite: 131].
    * **Garbage Collection:** Time-based hysteresis.

### B. Importers
* **ModelImporter:** Standardizes coords (Y-Up), MikkTSpace tangents, MeshOptimizer.
* **TextureImporter:** BC7/BC5 compression with Mipmaps.

---

## 3. Rendering Architecture (Vulkan 1.3)

**Role:** Explicit, high-performance, bindless.

### A. The Render Graph (Frame Graph)
[cite_start]**Role:** Automates synchronization and transient memory aliasing[cite: 106].
* **Structure:** DAG of `RenderPass` (Logic) + `Resource` (Data).
* **Compilation:**
    * [cite_start]Topological Sort -> Barrier Injection (`vkCmdPipelineBarrier2`)[cite: 207].
    * [cite_start]**Memory Aliasing:** Allocates non-overlapping transient textures (e.g., Depth, G-Buffer) in the same physical `DeviceMemory` block to save VRAM[cite: 116].
* **Execution:**
    * [cite_start]Graph nodes are dispatched as **Parallel Jobs**[cite: 113].
    * [cite_start]**Command Recording:** Workers record into Secondary Command Buffers[cite: 76].

### B. Shader & Material System (Bindless)
**Role:** Decoupled data binding.
* **Global Heap:**
    * [cite_start]Single `DescriptorSet` (Set 0) binding global arrays of Textures/Buffers (`binding = 10`, `VK_EXT_descriptor_indexing`)[cite: 220].
    * **Update Strategy:** Updates handled via `StagingBuffer` and aliased to the set.
* **Material Data:**
    * [cite_start]Materials are POD structs containing `uint32_t textureIndex`[cite: 223].
    * Data is uploaded to a massive `StorageBuffer` (MaterialBuffer).
    * **Push Constants:** Shader receives `materialID` and fetches data manually.
* **Benefit:** No `vkCmdBindDescriptorSets` per object. [cite_start]Massive CPU perf gain[cite: 227].

### C. Backend & Synchronization
* [cite_start]**Dynamic Rendering:** `vkCmdBeginRendering` (No `VkRenderPass` objects)[cite: 246].
* **Synchronization:**
    * **Timeline Semaphores:** Unifies Compute/Graphics/Transfer sync. [cite_start]Replaces `vkWaitForFences` entirely[cite: 88, 245].
    * [cite_start]**Poller Job:** "Wait-Free" GPU synchronization on CPU[cite: 92].
* **Frame Data:**
    * [cite_start]Triple Buffered `FrameContext` (CommandAllocators, DeletionQueue)[cite: 83].
    * [cite_start]**Context-Carried Pools:** `CommandAllocator` travels *with the Fiber* (via `JobContext`) or is grabbed from a thread-safe pool[cite: 67].

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

### Phase 5: Vulkan Architecture Refactor (Current Focus)
**Goal:** Eliminate race conditions and CPU stalls.

1.  **Fiber Safety & Core (Critical):**
    - [ ] [cite_start]**Stack Guard Pages:** Implement `PAGE_NOACCESS` between fiber stacks to trap overflows[cite: 51].
    - [x] **TLS Audit:** Search/Destroy `thread_local`. [cite_start]Replace with `JobContext` lookups[cite: 151].
    - [ ] [cite_start]**Job Concepts:** Implement C++20 Concepts (`JobPayload`) to ban non-trivial destructors in jobs[cite: 138].
2.  **Frame Synchronization (The Poller):**
    - [ ] **Timeline Wrapper:** Abstract `VK_KHR_timeline_semaphore`.
    - [ ] [cite_start]**Poller Job:** Implement the `VulkanWaitJob` that yields instead of blocks[cite: 178].
    - [ ] **Removal of Fences:** Delete all `vkWaitForFences` calls in the hot path.
3.  **Command Management:**
    - [ ] [cite_start]**Command Allocators:** Create pool of `CommandAllocator` (Pool + Cache) that can be claimed by a Job[cite: 68].
    - [ ] [cite_start]**Parallel Recording:** Dispatch RenderGraph passes to worker threads via Secondary Buffers[cite: 78].
4.  **Data Transfer:**
    - [ ] [cite_start]**Async Upload:** Implement `UploadContext` with a dedicated Transfer Queue and Staging Ring Buffer[cite: 131].

### Phase 6: Rendering Features (Bindless)
1.  **Bindless Pipeline:**
    - [ ] **Global Descriptor Set:** Setup `layout(binding=10) uniform texture2D globalTextures[];`.
    - [ ] [cite_start]**Material System Refactor:** Change materials to store `uint` indices instead of `VkDescriptorSet`[cite: 220].
2.  **PBR & Lighting:**
    - [ ] Environment Map Importer (.hdr).
    - [ ] IBL Compute Shaders.
    - [ ] **Shadows:** Cascaded Shadow Maps (CSM).

### Phase 7: Editor Polish & Gameplay
1.  **Scene Serialization:** YAML Save/Load.
2.  **Play/Stop Mode:** Scene state backup/restore.
3.  **Physics:** Jolt Integration.
4.  **Scripting:** C# Mono Integration.
