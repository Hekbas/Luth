# Luth Engine Architecture & Development Plan

**Status:** Active
**Philosophy:** Data-Oriented, Jobified, Vulkan-First.
 
---

## 0. System Hierarchy & Data Flow

### Architecture Tree
```text
[Engine Root]
 ├── [Core]
 │    ├── JobSystem (Fibers) ................... N:M Task Scheduler (Work Stealing)
 │    │    ├── Fiber Context ................... Stack management, FLS (Fiber Local Storage)
 │    │    └── Synchronization ................. Atomic Counters, Spinlocks, Timeline Semaphores
 │    ├── EventBus (Thread-Safe) ............... Inter-system communication
 │    └── Memory ............................... Allocators
 │         ├── LinearAllocator ................. Frame-local scratch memory (Frame Packets)
 │         └── PoolAllocator ................... Fixed-size components/fibers
 │
 ├── [Asset Pipeline]
 │    ├── AssetDatabase ........................ Metadata registry (Assets/ -> UUID)
 │    ├── Library .............................. Binary artifacts cache (Disk)
 │    └── AssetManager ......................... Runtime lifecycle
 │         ├── Importers ....................... Convert Source -> Artifact
 │         └── UploadContext ................... Async CPU -> GPU Transfer
 │              └── StagingBuffer .............. Ring Buffer (CPU_TO_GPU)
 │
 ├── [Scene / ECS]
 │    ├── Registry (EnTT) ...................... Entity/Component storage
 │    └── Systems .............................. Logic execution
 │         ├── TransformSystem ................. Hierarchy propagation (Parallel Level-Order)
 │         ├── CameraSystem .................... View/Projection calculation
 │         └── RenderingSystem ................. Scene culling & Graph submission
 │
 └── [Rendering (Vulkan 1.3)]
      ├── RenderGraph .......................... Automatic barriers & transient memory
      ├── ResourceCache ........................ Reuse of render targets/buffers
      ├── Bindless Descriptors ................. Global texture array (Set 0)
      └── Backend .............................. Device abstraction
           ├── Swapchain ....................... Presentation
           ├── FrameData ....................... Per-frame resources (CmdPools, Semaphores)
           └── Synchronization ................. Timeline Semaphores, Poller Jobs
```

### Execution Flow (Per Frame)
1.  **Input**: Poll GLFW -> Dispatch Events -> Update Input State.
2.  **Asset Sync**: Process `UploadContext` (Copy Staging -> Device Local).
    *   *Constraint:* Must happen before rendering to ensure resources are valid.
3.  **Logic Update**:
    *   `AssetManager`: Dispatch load jobs (Disk I/O -> Staging).
    *   `TransformSystem`: Update dirty hierarchies (Parallel Level-Order).
    *   `CameraSystem`: Recalculate matrices.
4.  **Render Prep**: `RenderingSystem` culls entities and adds passes to `RenderGraph`.
5.  **Render Execute**:
    *   `RenderGraph`: Compile (Calculate Barriers) -> Allocate Resources.
    *   `Backend`: Acquire Image -> Execute Graph (Record Cmds) -> Submit -> Present.
    *   *Constraint:* Non-blocking. CPU submits and moves to next frame. Sync via Timeline Semaphores (Poller Job).

---

## 1. Core Architecture

### A. The Fiber System (Job System)
**Role:** Abstracts CPU cores into a pool of workers executing Fibers.
**Philosophy:** The "Main Thread" is reserved strictly for OS Event Polling and Swapchain Presentation. All engine logic (Update, Physics, Render Recording) runs as Jobs.

*   **Primitives:**
    *   `Fiber`: Lightweight execution context (stack).
        *   *Implementation:* Windows Fibers initially, custom assembly later for perf.
        *   *Safety:* Guard pages between stacks to trap overflows.
    *   `Counter`: Atomic synchronization primitive for waiting.
    *   `JobQueue`: Lock-free Chase-Lev work-stealing deque per worker thread.
*   **Execution Model:**
    *   **Fork/Join:** `Dispatch(N)` splits work into chunks. Main thread waits on a counter.
    *   **Dependency Graph:** Jobs spawn child jobs. Parent waits on children.
    *   **Wait Strategy:** `WaitForCounter` does **not** sleep the thread. It switches the thread execution to a new Fiber from the pool to keep the core busy ("Fiber Yielding").
*   **Safety:**
    *   **TLS Hazard:** `thread_local` is forbidden or must be wrapped via `JobContext`.
    *   **Floating Point:** Respect ABI callee-saved registers (XMM6-XMM15) during context switch.

### B. Memory Management
**Role:** Minimize allocations during the frame loop.
*   **Linear Allocator (Frame Allocator):**
    *   Reset at the start of every frame.
    *   Used for: RenderGraph nodes, Command Lists, UI transient data, per-frame event data.
*   **Staging Buffer (Ring Buffer):**
    *   Persistent CPU-mapped buffer (`VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT`).
    *   Used for: Async texture/buffer uploads.
*   **Pool Allocator:**
    *   Used for: Components, Fibers, fixed-size objects.

### C. Event System
**Role:** Decouple systems (Input -> GameLogic, Window -> Renderer).
*   **Architecture:** Bus-based, Immediate or Buffered.
*   **Thread Safety:** Events generated on Main Thread (OS events) are buffered.

### D. Debugging & Profiling
*   **Tracy Integration:** Full instrumentation of Fibers (Context Switches), Locks, and GPU zones.
*   **Stuck Job Detector:** Watchdog to detect deadlocks or infinite loops in fibers.
*   **Console:** In-game/Editor console for logging and CVars.

---

## 2. Asset Pipeline

**Role:** Asynchronous loading, processing, and management of resources.

### A. Asset Database (Registry)
*   **Metadata:** Stores `Path`, `Type`, `UUID` in `.meta` files side-by-side with assets.
*   **Registry:** `std::unordered_map<UUID, AssetMetadata>`. Loaded at startup.
*   **Artifact Cache (Library):**
    *   Source assets (`.fbx`, `.png`) are imported into engine-ready binary formats stored in `Library/Artifacts/`.
    *   Runtime loads exclusively from `Library/`.

### B. Asset Manager (Runtime)
*   **Async Loading:**
    1.  `LoadAsync(UUID)` checks cache.
    2.  If missing, spawns a **Load Job**.
    3.  **Load Job** (Worker):
        *   Reads Artifact from disk.
        *   Allocates Staging Memory.
        *   Copies data to Staging.
        *   Pushes `UploadRequest` to `UploadContext`.
    4.  **Upload Phase** (Main Thread Start):
        *   Records `vkCmdCopyBufferToImage` for all pending requests.
        *   Submits to Transfer Queue (or Graphics Queue with barrier).
*   **Garbage Collection:** Time-based hysteresis (e.g., unload if unused for 5s).

### C. Importers
*   **ModelImporter:**
    *   **Geometry:** Standardizes coordinate system (Y-Up, Right-Handed). Generates Tangents/Normals (MikkTSpace).
    *   **Optimization:** Vertex cache optimization, overdraw reduction (meshoptimizer).
    *   **Hierarchy:** Extracts Skeleton/Bone hierarchy for animation.
    *   **Materials:** Extracts embedded materials and textures as separate sub-assets.
*   **TextureImporter:**
    *   **Compression:** BC7 (Color), BC5 (Normal), BC6H (HDR).
    *   **Mipmaps:** Box/Kaiser filter generation.
*   **MaterialImporter:**
    *   JSON-based definition.
    *   Maps standard PBR inputs (Albedo, Normal, Roughness, Metalness, AO) to Shader Uniforms.

---

## 3. Rendering Architecture (Vulkan)

**Role:** High-performance, parallel-friendly rendering.

### A. The Render Graph (Frame Graph)
**Role:** Automates synchronization (Barriers) and memory management (Transient Resources).
*   **Structure:** `RenderPass` (Logic) + `Resource` (Data).
*   **Execution Flow:** Setup -> Compile (Barriers/Aliasing) -> Execute.
*   **Transient Resources:** Textures needed only for the frame (e.g., DepthBuffer, GBuffer) are allocated from a specific `FrameHeap` and reused via aliasing.

### B. Shader & Material System
**Role:** Data-driven pipeline state and resource binding.
*   **Shader Asset:**
    *   Compiles GLSL -> SPIR-V.
    *   **Reflection (SPIRV-Cross):** Automatically determines Descriptor Set Layouts and Push Constant ranges.
    *   **Variants:** Uber-shader approach with `#define` permutations (STATIC_MESH, SKINNED, INSTANCED).
*   **Material Asset:**
    *   **PBR Standard:** Albedo, Normal, Metallic, Roughness, AO, Emissive.
    *   **Workflow:** Metallic-Roughness.
    *   Stores a binary blob of Uniform Data (UBO) matching the shader's reflection.
*   **Bindless Design:**
    *   **Global Bindless Set:** All textures in the engine are bound to a single Descriptor Array (Set 0).
    *   **Indices:** Materials store an integer index into this array.

### C. Lighting & Environment
*   **Global Illumination:** Image Based Lighting (IBL) for PBR (Irradiance Map + Prefiltered Environment Map + BRDF LUT).
*   **Shadows:** Cascaded Shadow Maps (CSM) for Directional Lights.
*   **Lights:** Directional, Point, Spot, Area (LTC).
*   **Post-Processing:** Tone Mapping (ACES), Bloom, SSAO, TAA.

### D. Backend (Vulkan 1.3)
*   **Dynamic Rendering:** No `VkRenderPass` objects. Use `vkCmdBeginRendering`.
*   **Synchronization 2:** Use `vkCmdPipelineBarrier2`.
*   **Frame Data (Double/Triple Buffering):**
    *   `VkCommandPool` (One per thread per frame).
    *   `VkSemaphore` (Timeline Semaphores for GPU-CPU sync).
    *   `DeletionQueue` (Resources to free when frame is complete).
*   **Upload Context:**
    *   Dedicated Command Buffer for uploads.
    *   Syncs via `UploadCompleteSemaphore`.

---

## 4. Scene System (ECS)

**Role:** Game logic and state management.
*   **Library:** EnTT.
*   **Entity:** `uint32_t` ID.
*   **Components:**
    *   `ID`, `Tag`: Identity.
    *   `Transform`, `WorldTransform`, `Parent`, `Children`: Hierarchy.
    *   `MeshRenderer`: Link to Model/Material assets.
    *   `Camera`: Projection data.
    *   `Light`: Directional/Point light data.
*   **Systems:**
    *   `TransformSystem`: Parallel Level-Order Traversal for hierarchy updates.
    *   `CameraSystem`: View/Projection matrix calculation.
    *   `RenderingSystem`: Culling and Draw Packet generation.
    *   `SceneSerializer`: YAML Save/Load.
    *    --- (Future) ---
    *   `AnimationSystem`: Updates bone matrices for Skinned Meshes.
    *   `ScriptSystem`: Updates C#/Lua scripts.
    *   `PhysicsSystem`: Integration with Jolt Physics.

---

## 5. Editor Architecture

**Role:** Tools for creation and debugging.
*   **UI Library:** Dear ImGui (Docking branch).
*   **Panels:** Scene, Hierarchy, Inspector, Project, Console, Profiler.
*   **Scene View:**
    *   **Gizmos:** ImGuizmo integration (Translate/Rotate/Scale).
    *   **Camera:** Fly/Orbit modes.
    *   **Picking:** Mouse picking via Framebuffer readback.
    *   **Grid:** Infinite grid rendering.
*   **Project Panel:**
    *   Virtual Filesystem view.
    *   Drag & Drop support.
    *   Thumbnail generation.
*   **Inspector:**
    *   Reflection-based component editing.
    *   Asset preview.

---

## 6. Implementation Roadmap

### Phase 1: Foundation (Done)
- [x] Context & Windowing (GLFW).
- [x] Vulkan Backend (Instance, Device, Swapchain, VMA).
- [x] Job System (Fibers, Counters).
- [x] ImGui Integration.
- [x] Input System.

### Phase 2: Data Pipeline (Done)
- [x] Shader Reflection (SPIRV-Cross).
- [x] Material System.
- [x] Asset Manager Async.

### Phase 3: Asset Cache & Editor Refactor (Done)
- [x] Asset Database & Artifacts.
- [x] Project Panel & Inspector.
- [x] Load-on-Inspect.

### Phase 4: ECS Architecture Refactor (Done)
- [x] POD Components.
- [x] Transform System (Parallel Hierarchy).
- [x] Camera System.

### Phase 5: Vulkan Architecture Refactor (Current Focus)
**Goal:** Eliminate race conditions and CPU stalls by implementing a robust Phase-Based rendering architecture.

1.  **Core Stability (Fiber Safety):**
    - [ ] **TLS Audit:** Replace `thread_local` usage with Fiber-Local Storage (FLS) via `JobContext` to prevent data corruption during migration.
    - [ ] **Stack Guard Pages:** Ensure fiber stacks are separated by `PAGE_NOACCESS` memory to trap overflows.
    - [ ] **Job Concepts:** Use C++20 Concepts to enforce constraints on Job Payloads.
2.  **Staging & Uploads (Async Transfer):**
    - [ ] **Staging Buffer:** Implement `StagingBuffer` class (Persistent Mapped Ring Buffer).
    - [ ] **Upload Context:** Implement `UploadContext` to manage `vkCmdCopy` commands on a Transfer Queue.
    - [ ] **Asset Integration:** Refactor `VKTexture` and `AssetManager` to write to Staging Buffer instead of blocking on `ImmediateSubmit`.
3.  **Frame Synchronization (Timeline Semaphores):**
    - [ ] **Timeline Wrapper:** Implement `VKTimelineSemaphore` abstraction.
    - [ ] **Frame Data:** Refactor `VKRendererAPI` to use explicit `FrameData` structs (CmdPools, Semaphores, DeletionQueue).
    - [ ] **Poller Job:** Implement a non-blocking Fiber Job that polls `vkGetSemaphoreCounterValue` and yields until GPU is ready.
4.  **Parallel Rendering:**
    - [ ] **Command Allocators:** Implement `CommandAllocator` pool (one per worker thread) to allow lock-free command buffer allocation inside jobs.
    - [ ] **Parallel Record:** Dispatch RenderGraph passes to worker threads using Secondary Command Buffers.
    - [ ] **Barrier Builder:** Implement `BarrierBuilder` helper for `vkCmdPipelineBarrier2` to simplify graph compilation.

### Phase 6: Rendering Features
1.  **PBR & IBL:**
    - [ ] Environment Map Importer (.hdr).
    - [ ] IBL Compute Shaders (Irradiance, Prefilter, BRDF).
    - [ ] PBR Shader Implementation.
    - [ ] **Material Parameters:** Expose Metallic/Roughness/AO factors in Inspector.
2.  **Shadow Mapping:**
    - [ ] Cascaded Shadow Maps (CSM).
    - [ ] Shadow Render Pass.
    - [ ] **PCF:** Implement Percentage Closer Filtering in PBR shader.

### Phase 7: Editor Polish & Gameplay
1.  **Scene Serialization:** YAML Save/Load.
2.  **Play/Stop Mode:** Scene state backup/restore.
3.  **Physics:** Jolt Integration.
4.  **Scripting:** C# Mono Integration.
