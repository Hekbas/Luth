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
    * *Constraint:* Must happen before rendering to ensure resources are valid.
3.  **Logic Update (Jobified)**:
    * **Kick Jobs**: Asset Loading, Physics, Transform Updates, Culling.
    * **Wait Strategy**: Main Thread yields to Worker Pool; never sleeps.
4.  **Render Prep**: `RenderGraph` compiles passes and injects `VkImageMemoryBarrier2`.
5.  **Render Execute**:
    * **Parallel Record**: Workers record into Secondary Command Buffers.
    * **Submit**: Main Thread submits to Graphics Queue.
    * **Present**: Standard Swapchain presentation.
    * **Constraint**: The CPU moves to Frame N+1 immediately. It checks Frame N's GPU completion via a **Poller Job** only when resource recycling is needed.

---

## 1. Core Architecture

### A. The Fiber System (Job System)
**Role:** Abstracts CPU cores into a pool of workers executing Fibers.
**Philosophy:** No blocking allowed. If a dependency is not met, the Fiber Yields.

* **Primitives:**
    * `Fiber`: Lightweight execution context (VirtualAlloc stack + Guard Page).
    * `JobContext`: The "Fiber Local Storage" (FLS). Contains pointer to current `FrameAllocator` and `CommandAllocator`.
    * `Counter`: Atomic variable for dependency tracking.
* **Execution Model:**
    * **Wait Strategy**: `WaitForCounter` switches execution to a new Fiber.
    * **Poller Job**: A specific job type that loops on `vkGetSemaphoreCounterValue`. If target not reached, it yields (re-queues itself) to allow other CPU work.
* **Safety Constraints:**
    * **NO `thread_local`**: Standard TLS is forbidden (unsafe due to migration). Use `JobContext`.
    * **Float Safety**: Respect ABI callee-saved registers (XMM6-XMM15) or enforce strict no-float zones across yields.

### B. Memory Management
* **Frame Packet Allocator (Linear):**
    * Allocates per-frame transient data (RenderGraph nodes, Draw packets).
    * **Rule**: POD types only. Destructors are **not** run.
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
* **Async Loading:**
    * Spawns **Load Job** (Disk I/O -> Staging).
    * Pushes to `UploadContext` (Transfer Queue).
    * **Garbage Collection:** Time-based hysteresis.

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

### C. Backend & Synchronization
* **Dynamic Rendering:** `vkCmdBeginRendering` (No `VkRenderPass` objects).
* **Synchronization:**
    * **Timeline Semaphores:** Unifies Compute/Graphics/Transfer sync. Replaces `vkWaitForFences` entirely 245].
    * **Poller Job:** "Wait-Free" GPU synchronization on CPU.
* **Frame Data:**
    * Triple Buffered `FrameContext` (CommandAllocators, DeletionQueue).
    * **Context-Carried Pools:** `CommandAllocator` travels *with the Fiber* (via `JobContext`) or is grabbed from a thread-safe pool.

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
    - [x] **Stack Guard Pages:** Implement `PAGE_NOACCESS` between fiber stacks to trap overflows.
    - [x] **TLS Audit:** Search/Destroy `thread_local`. Replace with `JobContext` lookups.
    - [x] **Job Concepts:** Implement C++20 Concepts (`JobPayload`) to ban non-trivial destructors in jobs.
2.  **Frame Synchronization (The Poller):**
    - [x] **Timeline Wrapper:** Abstract `VK_KHR_timeline_semaphore`.
    - [x] **Poller Job:** Implement the `VulkanWaitJob` that yields instead of blocks.
    - [ ] **Removal of Fences:** Delete all `vkWaitForFences` calls in the hot path.
3.  **Command Management:**
    - [ ] **Command Allocators:** Create pool of `CommandAllocator` (Pool + Cache) that can be claimed by a Job.
    - [ ] **Parallel Recording:** Dispatch RenderGraph passes to worker threads via Secondary Buffers.
4.  **Data Transfer:**
    - [ ] **Async Upload:** Implement `UploadContext` with a dedicated Transfer Queue and Staging Ring Buffer.

### Phase 6: Rendering Features (Bindless)
1.  **Bindless Pipeline:**
    - [ ] **Global Descriptor Set:** Setup `layout(binding=10) uniform texture2D globalTextures[];`.
    - [ ] **Material System Refactor:** Change materials to store `uint` indices instead of `VkDescriptorSet`.
2.  **PBR & Lighting:**
    - [ ] Environment Map Importer (.hdr).
    - [ ] IBL Compute Shaders.
    - [ ] **Shadows:** Cascaded Shadow Maps (CSM).

### Phase 7: Editor Polish & Gameplay
1.  **Scene Serialization:** YAML Save/Load.
2.  **Play/Stop Mode:** Scene state backup/restore.
3.  **Physics:** Jolt Integration.
4.  **Scripting:** C# Mono Integration.
