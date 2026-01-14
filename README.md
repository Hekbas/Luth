<p align="center">
  <img src="docs/screenshots/luth_logo.png" alt="Luth Logo" width="350"/>
</p>

<p align="center">
  <a href="https://github.com/Hekbas/Luth/actions"><img alt="Build Status" src="https://img.shields.io/github/actions/workflow/status/Hekbas/Luth/build.yml?style=for-the-badge"></a>
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows-informational.svg?style=for-the-badge">
  <img alt="Language" src="https://img.shields.io/badge/language-C++20-blue.svg?style=for-the-badge">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-green.svg?style=for-the-badge">
  <img alt="Status" src="https://img.shields.io/badge/status-Active%20Development-green.svg?style=for-the-badge">
</p>

<p align="center">
  <b>C++ game engine built to explore high-performance architecture.</b><br>
  Currently under active development, serves as both a learning platform and research project.
</p>
<p align="center">
  Or it might just be a playground to test my sanity.
</p>

> [!IMPORTANT]
> <i>My original Bachelor's Thesis version is archived in the <code>thesis</code> branch.</i>

<p align="center">
  <img src="docs/screenshots/LuthEditor.png" alt="Engine Screenshot" style="border-radius: 8px;"/>
</p>

---

## Why Luth?

Honestly? I just really love this stuff.

It started with my Bachelor's Thesis. I built a "functional C++ engine" from scratch. It was single-threaded and used a very basic Vulkan implementation (no bindless resources, no complex concurrency) just a simple loop. It worked, and I had a blast building it!

Then I watched Christian Gyrling’s GDC talk on *[Parallelizing the Naughty Dog Engine Using Fibers](https://www.gdcvault.com/play/1022186/Parallelizing-the-Naughty-Dog-Engine)*.

It blew my mind. Seeing how they saturated every single CPU core made me realize that my "simple loop" was basically running with the parking brake on. I really wanted to understand the raw, complex machinery behind AAA engines.

So, I scrapped my old architecture and started Luth from scratch.

This project is my personal deep dive into fiber-based job systems, lock-free memory models, bindless Vulkan rendering... It is absolutely over-engineered for a solo project, but that’s the point. I am building this to learn, to fail, and to obsess over the details that turn good code into high-performance architecture.

---

## Technical Architecture

Luth moves away from standard C++ patterns (RAII everywhere, heavy STL usage, single-threaded contexts) in favor of **Data-Oriented Design** and **Fiber-Based Concurrency**.

### 1. The Fiber Job System
Instead of spawning OS threads for specific tasks (like a "Render Thread" or "Audio Thread"), Luth treats the CPU as a generic pool of workers.
* **N:M Threading:** The engine spawns one Worker Thread per CPU core. Logical tasks are wrapped in **Fibers** (lightweight user-mode stacks) that can migrate between workers.
* **Zero Blocking:** The engine is designed to never sleep. If a job needs to wait for a dependency (or the GPU), it yields execution to the scheduler, which immediately swaps in another fiber. This keeps CPU saturation near 100%.
* **Naughty Dog Inspiration:** The scheduler implementation utilizes **Adaptive Mutexes** (spinning briefly before yielding) and **Atomic Counters** for synchronization to avoid priority inversion.

### 2. Pipelined Frame Execution
To hide latency, the engine pipelines execution across three distinct stages running in parallel. At any given moment $T$, the engine is processing three frames simultaneously:
1.  **Frame N (Game Logic):** Physics, AI, and Transform updates run on the CPU.
2.  **Frame N-1 (Render Logic):** The results of the previous game frame are read to record Vulkan Command Buffers in parallel.
3.  **Frame N-2 (GPU execution):** The GPU executes the commands submitted for the frame prior.

### 3. Memory Strategy
Standard `new`/`delete` calls are forbidden in the hot path. Luth uses a strict memory hierarchy to handle the complexity of fiber migration:
* **Tagged Page Allocator:** A Naughty Dog-style allocator using 2MB virtual pages. Memory is allocated with a specific "Tag" (e.g., `LevelGeometry`, `Frame_N`) and freed in bulk. It uses per-thread caches to allow lock-free allocations during gameplay.
* **Frame Packets:** Linear allocators that reset every frame. Used for transient data like command lists or UI state. This eliminates destructor overhead for 90% of runtime objects.

### 4. Vulkan 1.3 Backend
The renderer is built for modern hardware, focusing on reducing driver overhead.
* **Bindless Descriptors:** Uses `VK_EXT_descriptor_indexing` to bind all engine textures to a single global array (`Set 0`). Materials simply store an integer index, allowing any draw call to access any texture without rebinding sets.
* **Dynamic Rendering:** Eliminates legacy `VkRenderPass` and `VkFramebuffer` objects.
* **Timeline Semaphores:** Replaces `vkWaitForFences`. A dedicated **Poller Job** runs on the CPU, querying semaphore values and waking up dependent fibers only when the GPU has finished a specific workload.

---

## Build Instructions

**Prerequisites:**
- **OS**: Windows 10/11
- **Compiler**: MSVC (v143+) or Clang (C++20 compliant)
- **SDK**: [Vulkan SDK 1.3+](https://vulkan.lunarg.com)

**Steps:**
1.  **Clone the repository + submodules**
    ```bash
    git clone --recursive https://github.com/Hekbas/Luth.git
    ```
2.  **Generate Project Files**:
    Run the setup script to run Premake:
    ```bash
    scripts/setup_windows.bat
    ```
3.  **Build**:
    Open the generated solution `Luth.sln` and build the project.

---

## Dependencies & Libraries

LUTH Engine is built on the shoulders of giants:

* [**Vulkan SDK**](https://www.lunarg.com/vulkan-sdk/): The core rendering backend.
* [**EnTT**](https://github.com/skypjack/entt): Fast Entity-Component-System (ECS).
* [**ImGui**](https://github.com/ocornut/imgui): Immediate Mode GUI for the Editor.
* [**Jolt Physics**](https://github.com/jrouwe/JoltPhysics): (Planned) High-performance rigid body physics.
* [**Tracy**](https://github.com/wolfpld/tracy): Real-time remote frame profiling.
* [**SPIRV-Cross**](https://github.com/KhronosGroup/SPIRV-Cross): Shader reflection and cross-compilation.
* [**GLFW**](https://www.glfw.org/): Windowing and Input management.
* [**GLM**](https://glm.g-truc.net/): Mathematics library.
* [**spdlog**](https://github.com/gabime/spdlog): Fast C++ logging.
* [**assimp**](https://github.com/assimp/assimp): Asset importing (Models).
* [**stb_image**](https://github.com/nothings/stb): Image loading.
* [**nlohmann/json**](https://github.com/nlohmann/json): JSON serialization.

---

## Detailed Roadmap
> [!WARNING]
> The engine is currently undergoing a massive architectural reboot (Phase 5) to fully implement the Fiber/Vulkan integration described above.

### Phase 1: Engine Bootstrap [Completed]
> Core platform abstraction and initial rendering capability.

- [x] **Platform Layer:**
  - [x] **Windowing:** GLFW integration with window abstraction.
  - [x] **Input System:** Centralized event bus and polling (Mouse/Keyboard/Controller).
  - [x] **Vulkan Bootstrap:** Initial Instance/Device creation and VMA (Vulkan Memory Allocator) integration.
- [x] **ImGui Integration:**
  - [x] **Viewports & Docking:** Multi-window support enabling a modern editor layout.
  - [x] **Vulkan Hooks:** Custom render backend for ImGui draw lists.

### Phase 2: Concurrency v1.0 [Completed]
> Foundation of the Job System (Pre-Reboot).

- [x] **Fiber Primitives:**
  - [x] **Task Scheduler:** Fiber-based execution model using assembly context switching.
  - [x] **Synchronization:** Basic counters and wait primitives.
- [x] **Async Asset Loading:** Job-based loading pipeline to offload disk I/O from the main thread.

### Phase 3: Data Pipeline & Artifacts [Completed]
> Robust, metadata-driven asset management system.

- [x] **Artifact System:**
  - [x] **Asset Database:** Integrity scanning, orphan cleanup, and UUID registry.
  - [x] **Library Cache:** "Source vs. Artifact" separation. Importers compile raw assets (PNG, OBJ) into efficient binary binaries stored in `Library/`.
  - [x] **Metadata:** Generation of `.meta` files for import settings stability.
- [x] **Shader Reflection:**
  - [x] **SPIRV-Cross Integration:** Automated reflection of shader binaries to generate Descriptor Set layouts and pipeline interfaces dynamically.
  - [x] **Material System:** Reflection-driven property exposure (uniforms matched to Inspector fields).

### Phase 4: ECS & Editor Architecture [Completed]
> Scalable entity management and WYSIWYG tooling.

- [x] **ECS Optimization:**
  - [x] **Dirty Flags:** Reactive transform updates to avoid redundant matrix multiplication.
  - [x] **Hierarchy Propagation:** Optimized O(N) parent-to-child world matrix calculation.
  - [x] **POD Enforcement:** Strict Plain-Old-Data layout for core components (`Transform`, `Camera`) to maximize cache locality.
- [x] **Editor Polish:**
  - [x] **Project Panel:** Virtual filesystem view with Search, Zoom, and Drag & Drop support.
  - [x] **Inspector:** Async preview loading and weak reference management for textures.
  - [x] **Gizmos:** ImGuizmo integration for visual object manipulation.

### Phase 5: Core Systems [Current Focus]
> Establish the memory and execution foundation. No Vulkan implementation in this phase.

**5.1 Memory Architecture**
- [ ] **Tagged Page Allocator:**
  - [ ] Implement `TaggedPageAllocator` (Pool of 2MB `VirtualAlloc` pages).
  - [ ] Implement `FreeTag(uint32_t tag)` for bulk reclamation.
  - [ ] Implement `ThreadCache` (Index-based access via `JobContext`) to minimize global lock contention.
- [ ] **Frame Architecture:**
  - [ ] Define `FrameParams` struct (Inputs, Time, Matrices, Viewport).
  - [ ] Implement Triple Buffering for `FrameParams` (Game N, Render N-1, GPU N-2).
- [ ] **Job Context:**
  - [ ] Define `struct JobContext`.
  - [ ] **Fields:** `TaggedAllocator*`, `FrameParams*`, `ThreadIndex`, `FiberID`.
  - [ ] **Rule:** All Jobs must accept `JobContext&` as their primary argument.

**5.2 Fiber Runtime**
- [ ] **Safety Audit:**
  - [ ] Remove all `thread_local` variables. Replace with `JobContext` lookups.
  - [ ] Verify `Guard Page` implementation (protects against stack overflow).
- [ ] **Synchronization Primitives:**
  - [ ] Implement `AdaptiveMutex` (Spin ~2000 cycles -> `Fiber::Yield()`).
  - [ ] Implement `AtomicCounter` (Wait adds fiber to "Wait List", resume moves to "Ready List").
- [ ] **Scheduler Upgrade:**
  - [ ] Implement **Priority Queues** (High, Normal, Low).
  - [ ] **Policy:** High Priority (Game Logic) preempts Low Priority (Asset Loading).

**5.3 Engine Loop**
- [ ] **Main Loop Refactor:**
  - [ ] Implement explicit pipeline staging: `KickGame(N)` -> `SubmitRender(N-1)` -> `RecycleGPU(N-2)`.
- [ ] **I/O Subsystem:**
  - [ ] Spawn dedicated `IO_Thread` (OS Thread).
  - [ ] Implement `FileRequestQueue` (Lock-free ring buffer).
  - [ ] **Constraint:** Blocking I/O (fread/fstream) forbidden in Fiber Workers.

---

### Phase 6: Vulkan Backend
> Lock-free, explicit command generation utilizing the core memory model.

**6.1 Synchronization**
- [ ] **Timeline Semaphores:**
  - [ ] Create `TimelineSemaphore` wrapper.
  - [ ] **Rule:** All Queue submits must signal a Timeline value (Frame Index).
- [ ] **The Poller Job:**
  - [ ] Implement `VulkanWaitJob`.
  - [ ] **Logic:** `vkGetSemaphoreCounterValue` -> If < Target, Yield; Else, execute callback.
  - [ ] **Cleanup:** Remove all `vkWaitForFences` calls.

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
> High-throughput resource streaming.

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
> Graphical fidelity.

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
> Engine usability.

- [ ] **Scene Serialization:** YAML Save/Load via EnTT.
- [ ] **Physics:** Jolt Physics Integration (Jobified via `JobSystem`).
- [ ] **Scripting:** C# Mono Integration (Game Logic stage).
- [ ] **Editor:** ImGui Docking, Gizmos, Asset Browser.