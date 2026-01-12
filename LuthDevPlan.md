# Luth Engine Architecture & Development Plan

**Status:** Active
**Philosophy:** Data-Oriented, Jobified, Vulkan-First.

---

## 1. Core Architecture

### A. The Fiber System (Job System)
**Role:** The central "heartbeat". Abstracts CPU cores into a pool of workers executing Fibers.
**Philosophy:** The "Main Thread" is reserved strictly for OS Event Polling and Swapchain Presentation. All engine logic (Update, Physics, Render Recording) runs as Jobs.

*   **Primitives:**
    *   `Fiber`: Lightweight execution context (stack).
    *   `Counter`: Atomic synchronization primitive for waiting.
    *   `JobQueue`: Lock-free (or fine-grained locked) queue of function pointers.
*   **Execution Model:**
    *   **Fork/Join:** `Dispatch(N)` splits work into chunks. Main thread waits on a counter.
    *   **Dependency Graph:** Jobs spawn child jobs. Parent waits on children.
    *   **Wait Strategy:** `WaitForCounter` does **not** sleep the thread. It switches the thread execution to a new Fiber from the pool to keep the core busy ("Fiber Yielding").
*   **Safety:**
    *   Jobs must be stateless or operate on exclusive data chunks.
    *   Global state access must be read-only or double-buffered.

### B. Memory Management
**Role:** Minimize allocations during the frame loop.
*   **Linear Allocator (Frame Allocator):**
    *   Reset at the start of every frame.
    *   Used for: RenderGraph nodes, Command Lists, UI transient data, per-frame event data.
    *   *Implementation:* `LinearAllocator` class (already exists).
*   **Pool Allocator:**
    *   Used for: Components, Fibers, fixed-size objects.
*   **Heap Allocator:**
    *   Used for: Long-lived assets (Textures, Meshes) loaded at startup/streaming.

### C. Event System
**Role:** Decouple systems (Input -> GameLogic, Window -> Renderer).
*   **Architecture:** Bus-based, Immediate or Buffered.
*   **Thread Safety:**
    *   Events generated on Main Thread (OS events) are buffered.
    *   Events are dispatched to Systems at specific sync points or consumed via `EventBus::ProcessEvents`.
    *   *Current State:* Thread-safe queue implemented.

---

## 2. Asset Pipeline

**Role:** Asynchronous loading, processing, and management of resources.

### A. Asset Database (Registry)
*   **Metadata:** Stores `Path`, `Type`, `UUID` in `.meta` files side-by-side with assets.
*   **Registry:** `std::unordered_map<UUID, AssetMetadata>`. Loaded at startup by scanning the `Assets/` folder.
*   **Hot Reloading:** `FileWatcher` detects changes. Triggers re-import on worker thread.
*   **Artifact Cache (Library):**
    *   Source assets (`.fbx`, `.png`) are imported into engine-ready binary formats stored in `Library/Artifacts/`.
    *   Import happens only when source hash or meta settings change.
    *   **Version Control:** `Library/` is excluded. `Assets/` and `.meta` files are committed.
    *   Runtime loads exclusively from `Library/`, never parsing raw formats like FBX

### B. Asset Manager (Runtime)
*   **Async Loading:**
    1.  `LoadAsync(UUID)` checks cache.
    2.  If missing, spawns a **Load Job**.
    3.  **Load Job** (Worker): 
        *   Checks if Artifact exists in `Library/`. If not, triggers **Import**.
        *   Deserializes the binary Artifact into `AssetData`.
    4.  **Upload Phase** (Main Thread): Creates GPU resources from `AssetData`.
    5.  **Garbage Collection:** Automatically unloads assets with no external references every 2 seconds.
*   **Reference Counting:** `std::shared_ptr` handles lifetime.

### C. Importers
*   **ModelImporter:**
    *   Standardizes coordinate system (Y-Up, Right-Handed).
    *   Generates Tangents/Normals.
    *   Extracts Materials and Textures as separate sub-assets (or embedded dependencies).
*   **TextureImporter:** Handles compression (BC7/BC5) and Mipmap generation.
*   **MaterialImporter:** JSON-based definition of shader parameters and texture slots.

---

## 3. Rendering Architecture (Vulkan)

**Role:** High-performance, parallel-friendly rendering.

### A. The Render Graph (Frame Graph)
**Role:** Automates synchronization (Barriers) and memory management (Transient Resources).
*   **Structure:**
    *   `RenderPass`: A logical unit of work (e.g., "ShadowPass", "GBufferPass", "LightingPass").
    *   `Resource`: Abstract handle (`ResourceHandle`) representing a Texture or Buffer.
*   **Execution Flow:**
    1.  **Setup:** Systems declare reads/writes to resources.
    2.  **Compile:** Graph calculates lifetimes, injects `vkCmdPipelineBarrier`, and aliases memory for transient textures.
    3.  **Execute:** Graph iterates passes.
        *   *Future Optimization:* Passes can be dispatched to Job Workers to record Command Buffers in parallel.
*   **Transient Resources:** Textures needed only for the frame (e.g., DepthBuffer, GBuffer) are allocated from a specific `FrameHeap` and reused.

### B. Shader & Material System (Priority)
**Role:** Data-driven pipeline state and resource binding.
*   **Shader Asset:**
    *   Compiles GLSL -> SPIR-V.
    *   **Reflection (SPIRV-Cross):** Automatically determines:
        *   Descriptor Set Layouts (Set 0: Global, Set 1: Material, Set 2: Object).
        *   Push Constant ranges.
        *   Vertex Input attributes.
*   **Material Asset:**
    *   **PBR Standard:** Albedo, Normal, Metallic, Roughness, AO, Emissive.
    *   **Workflow:** Metallic-Roughness.
    *   Holds a reference to a `Shader`.
    *   Stores a binary blob of Uniform Data (UBO) matching the shader's reflection.
    *   Stores references to Textures.
*   **Bindless Design:**
    *   **Global Bindless Set:** All textures in the engine are bound to a single Descriptor Array (e.g., `binding = 10[]`).
    *   Materials store an *index* into this array, passed to the shader via Push Constants or UBO.

### C. Lighting & Environment
*   **Global Illumination:** Image Based Lighting (IBL) for PBR. (Irradiance Map + Prefiltered Environment Map + BRDF LUT).
*   **Shadows:** Cascaded Shadow Maps (CSM) for Directional Lights.

### C. Backend (Vulkan)
*   **Dynamic Rendering:** No `VkRenderPass` or `VkFramebuffer` objects. Use `vkCmdBeginRendering`.
*   **Memory:** VMA (Vulkan Memory Allocator).
*   **Synchronization:** `vkCmdPipelineBarrier2` (Synchronization2 extension).

---

## 4. Scene System (ECS)

**Role:** Game logic and state management.
*   **Library:** EnTT.
*   **Entity:** Just a `uint32_t` ID.
*   **Components:** POD structs (`Transform`, `MeshRenderer`, `Camera`, `Light`).
*   **Systems:**
    *   `TransformSystem`: Computes World Matrices from Local Matrices + Hierarchy. (Parallelizable).
    *   `RenderingSystem`: Culls objects, submits draw calls to RenderGraph.
    *   `SceneSerializer`: Saves/Loads the Entity Registry to YAML/JSON.
    *   `ScriptSystem`: Updates C#/Lua scripts (Future).

---

## 5. Editor Architecture

**Role:** Tools for creation and debugging.
*   **UI Library:** Dear ImGui (Docking branch).
*   **State:**
    *   **Edit Mode:** Editor Camera controls view. Systems paused (except Rendering).
    *   **Play Mode:** Game Camera controls view. Physics/Scripts active.
*   **Panels:**
    *   `ScenePanel`: Renders the `SceneColor` texture from the RenderGraph into an ImGui window. Handles Gizmos (ImGuizmo).
    *   `HierarchyPanel`: Tree view of Entities. Handles parenting/reordering.
    *   `InspectorPanel`: Reflection-based editing of Components and Assets.
    *   `ContentBrowser`: File system view. Handles Drag & Drop of assets.
    *   `ConsolePanel`: Captures engine logs (spdlog sink) and displays them in the editor.

---

## 6. Implementation Roadmap

### Phase 1: Foundation (Done/In-Progress)
- [x] Context & Windowing (GLFW).
- [x] Vulkan Backend (Instance, Device, Swapchain).
- [x] Job System (Fibers).
- [x] ImGui Integration.
- [x] Input System (Event Bus).

### Phase 2: Data Pipeline (Done)
- [x] **Shader Reflection:** Implement `Shader` class using SPIRV-Cross to generate layouts automatically.
- [x] **Material System Refactor:** Update `Material` to use generic data buffers defined by the Shader.
- [x] **Asset Manager Async:** Ensure `LoadAsync` works robustly with the Job System.

### Phase 3: Asset Cache & Editor Refactor (Current Focus)
 **Goal:** Implement a Unity-style Library cache to eliminate runtime parsing overhead.
  
 1.  **Asset Database Integrity:** [Done]
 2.  **Project Panel Rewrite:** [Done]
 3.  **Artifact System:** [Done]
     *   Implemented `AssetSerializer` for binary formats.
     *   Updated `AssetManager` to load from `Library/` artifacts.
     *   Updated Importers to compile source assets to artifacts.
 4.  **Inspector & Selection:** Update to load from Library. [Done]

### Phase 4: Render Graph & Scene (Current Focus)
1.  **Scene Rendering:** Connect ECS `MeshRenderer` to the RenderGraph. [Done]
2.  **Transient Resources:** Implement aliasing/reuse in RenderGraph for GBuffer/Depth. [Done]
3.  **Bindless Textures:** Finalize the global texture array integration and shader usage. [Done]
4.  **PBR & IBL:** Implement Physically Based Rendering and Image Based Lighting. [Current Focus]
5.  **Shadow Mapping:** Implement a Shadow Pass in the Render Graph.

### Phase 5: Editor Polish
1.  **Play/Stop State:** Implement the simulation loop toggle.
2.  **Gizmos:** Integrate ImGuizmo for Transform manipulation. [Done]
3.  **Scene Serialization:** Save and Load Scenes (YAML).
4.  **Picking:** Implement Mouse Picking (Entity selection via viewport click).
5.  **Console Panel:** In-editor log viewer.
6.  **Frame Debugger:** Visualizer for RenderGraph passes and resources.

### Phase 6: Gameplay Features
1.  **Physics:** Integrate Jolt or PhysX.
2.  **Scripting:** Integrate Mono (C#) or Lua.
