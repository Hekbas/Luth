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

### D. Debugging & Profiling
*   **Tracy Integration:** Full instrumentation of Fibers, Locks, and GPU zones.
*   **Crash Handler:** Stack trace capture and dump generation.
*   **Console:** In-game/Editor console for logging and CVars (Command Variables).

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
    *   Generates Tangents/Normals (MikkTSpace).
    *   Extracts Materials and Textures as separate sub-assets (or embedded dependencies).
    *   **Mesh Optimization:** Vertex cache optimization, overdraw reduction (meshoptimizer).
    *   **LOD Generation:** Auto-generate simplified meshes.
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
    *   **Variants:** Uber-shader approach with `#define` permutations (STATIC_MESH, SKINNED, INSTANCED).
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
*   **Shadows:** Cascaded Shadow Maps (CSM) for Directional Lights. Point Light Shadows (Cubemaps).
*   **Lights:**
    *   Directional Light (Sun).
    *   Point Lights (with radius/falloff).
    *   Spot Lights (with cone angles).
    *   Area Lights (LTC - Linearly Transformed Cosines).
*   **Post-Processing:**
    *   Tone Mapping (ACES).
    *   Bloom (Compute Shader downsample/upsample).
    *   SSAO (Screen Space Ambient Occlusion) / GTAO.
    *   FXAA / TAA.
    *   Depth of Field.

### D. Backend (Vulkan 1.3)
*   **Dynamic Rendering:** No `VkRenderPass` or `VkFramebuffer` objects. Use `vkCmdBeginRendering`.
*   **Memory:** VMA (Vulkan Memory Allocator).
*   **Synchronization:** `vkCmdPipelineBarrier2` (Synchronization2 extension).
*   **Pipeline Cache:** Save/Load `VkPipelineCache` to disk to speed up startup.

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
    *   `PhysicsSystem`: Integration with Jolt Physics.
    *   `AnimationSystem`: Updates bone matrices for Skinned Meshes.

### A. Transform System Details
*   **Local vs World:** Components store Local Position/Rotation/Scale. System computes World Matrix.
*   **Dirty Flags:** Only recompute hierarchy if parent or self changed.
*   **Optimization:** Store matrices in SoA (Structure of Arrays) for SIMD processing.

### B. Animation System Details
*   **Skeleton:** Hierarchy of bones (Entities or internal nodes).
*   **Clips:** Keyframe data (Translation, Rotation, Scale) over time.
*   **Animator:** State Machine (Idle -> Walk -> Run) with blending.
*   **Skinning:** Compute `MatrixPalette` (Array of Mat4) -> Upload to GPU (SSBO) -> Vertex Shader.

 ---

## 5. Editor Architecture

**Role:** Tools for creation and debugging.
*   **UI Library:** Dear ImGui (Docking branch).
*   **State:**
    *   **Edit Mode:** Editor Camera controls view. Systems paused (except Rendering).
    *   **Play Mode:** Game Camera controls view. Physics/Scripts active. Scene state is cloned/snapshotted to allow revert on Stop.
*   **Panels:**
    *   `ScenePanel`: Renders the `SceneColor` texture from the RenderGraph into an ImGui window. Handles Gizmos (ImGuizmo).
    *   `HierarchyPanel`: Tree view of Entities. Handles parenting/reordering.
    *   `InspectorPanel`: Reflection-based editing of Components and Assets.
    *   `ContentBrowser`: File system view. Handles Drag & Drop of assets.
    *   `ConsolePanel`: Captures engine logs (spdlog sink) and displays them in the editor.
    *   `GamePanel`: Renders the active Game Camera view (no gizmos).
    *   `ProfilerPanel`: Real-time graphs of CPU/GPU usage.

### A. Scene Viewport Features
*   **Editor Camera:**
    *   **Fly Mode:** WASD + Right Click.
    *   **Orbit Mode:** Alt + Left Click around focus point.
    *   **Pan:** Middle Click.
    *   **Focus:** 'F' key to frame selected object.
*   **Gizmos:**
    *   Select/Translate/Rotate/Scale.
    *   Local/Global coordinate toggle.
    *   Snapping (Grid/Angle).
*   **Grid System:**
    *   Infinite Grid shader (fade at horizon).
    *   Adaptive scale (lines appear/disappear as you zoom).
*   **Selection:**
    *   **Mouse Picking:** Render Entity IDs to an offscreen integer framebuffer. Read pixel under mouse to get Entity ID.
    *   **Marquee Selection:** Select multiple objects.
*   **Debug Draw:**
    *   Wireframe overlay.
    *   Physics colliders.
    *   Light frustums/spheres.

 ---

## 6. Implementation Roadmap
 
 ### Phase 1: Foundation
 - [x] **Context & Windowing:** GLFW integration, Window abstraction.
 - [x] **Vulkan Backend:** Instance, Device, Swapchain, VMA Allocator.
 - [x] **Job System:** Fiber-based task scheduler, Counters, Wait primitives.
 - [x] **ImGui Integration:** Docking, Viewports, Vulkan backend hooks.
 - [x] **Input System:** Event Bus, Centralized Input polling.
 
 ### Phase 2: Data Pipeline
 - [x] **Shader Reflection:** SPIRV-Cross integration, automatic Descriptor Set layout generation.
 - [x] **Material System:** Generic uniform buffers, dynamic property access based on reflection.
 - [x] **Asset Manager Async:** Job-based loading, duplicate request handling.
 
 ### Phase 3: Asset Cache & Editor Refactor
 - [x] **Asset Database Integrity:** Startup scan, meta file generation, orphan cleanup.
 - [x] **Project Panel Rewrite:** Virtual filesystem view, Drag & Drop, Search, Zoom.
 - [x] **Artifact System:**
     - [x] `AssetSerializer` for binary formats (Texture/Model/Material).
     - [x] `Library/` folder management.
     - [x] Importers writing to Artifacts.
     - [x] Startup Import Phase (Parallelized).
 - [x] **Inspector & Selection:** Load-on-Inspect, Async preview loading, Weak references for textures.
 
 ### Phase 4: ECS Architecture Refactor (Current Focus)
 **Goal:** Eliminate technical debt in the Entity Component System to ensure scalability and maintainability.
 
 1.  **Component Cleanup:**
     - [x] **POD Enforce:** Refactor `Camera`, `Transform` to be strict POD (Plain Old Data). Move logic to Systems.
     - [x] **Dirty Flags:** Implement dirty state for Transforms to avoid redundant matrix recalculations.
 2.  **Transform System:**
     - [x] **Hierarchy Propagation:** Implement efficient parent-to-child world matrix updates (O(N) instead of O(N*Depth)).
     - [x] **Parallelization:** Ensure hierarchy updates are jobified correctly (Level-order traversal).
 3.  **Camera System:**
     - [ ] **System-Driven:** Move projection calculation from Component to `CameraSystem`.
 
 ### Phase 5: Render Graph & Scene
 **Goal:** Implement high-fidelity PBR rendering.
 
 1.  **Scene Rendering:**
     - [x] Connect ECS `MeshRenderer` to RenderGraph.
     - [x] Transient Resources (Aliasing/Reuse).
     - [x] Bindless Textures (Global Array).
 2.  **PBR & IBL (Physically Based Rendering):**
     - [ ] **Environment Asset:** Importer for `.hdr` files.
     - [ ] **IBL Generation:** Compute Shaders to generate Irradiance Map, Prefiltered Environment Map, and BRDF LUT.
     - [ ] **PBR Shader:** Implement Cook-Torrance BRDF in `triangle.frag`.
     - [ ] **Material Parameters:** Expose Metallic/Roughness/AO factors in Inspector.
 3.  **Shadow Mapping:**
     - [ ] **Directional Light:** Implement Cascaded Shadow Maps (CSM).
     - [ ] **Shadow Pass:** Add a dedicated pass in `RenderGraph` for shadow depth generation.
     - [ ] **PCF:** Implement Percentage Closer Filtering in PBR shader.
 
 ### Phase 6: Editor Polish
 **Goal:** Make the editor usable for actual level design.
 
 1.  **Scene Serialization:**
     - [ ] **YAML Serializer:** Save/Load Entity hierarchy, Components, and Asset references.
     - [ ] **Scene File Handling:** Double-click `.luthscene` assets to load.
 2.  **Play/Stop State:**
     - [ ] **Scene Copy:** Backup scene state on Play, restore on Stop.
     - [ ] **Runtime Camera:** Switch view from Editor Camera to active Scene Camera.
 3.  **Selection & Gizmos:**
     - [x] ImGuizmo Integration.
     - [ ] **Mouse Picking:** Framebuffer readback for pixel-perfect selection.
     - [ ] **Marquee Selection:** Select multiple entities.
 4.  **Editor Tools:**
     - [ ] **Grid System:** Infinite grid shader in Scene View.
     - [ ] **Console Panel:** Capture `spdlog` output to an ImGui window.
     - [ ] **Frame Debugger:** Visualize RenderGraph topology and resource states.
 
 ### Phase 7: Gameplay Features
 **Goal:** Enable game logic and interaction.
 
 1.  **Physics System (Jolt):**
     - [ ] **Integration:** Initialize Jolt Physics system.
     - [ ] **Components:** RigidBody, BoxCollider, SphereCollider, CapsuleCollider, MeshCollider.
     - [ ] **Debug Draw:** Wireframe rendering of colliders.
 2.  **Scripting System (C# Mono):**
     - [ ] **Mono Host:** Embed Mono runtime.
     - [ ] **Script Component:** Bind C# classes to Entities.
     - [ ] **Internal Calls:** Expose Transform, Input, and Log API to C#.
 3.  **Animation System:**
     - [ ] **Skinned Mesh:** Re-enable and refactor `SkinnedModel`.
     - [ ] **Animator:** State machine for playing clips.
     - [ ] **Skinning Shader:** Vertex shader bone transform logic.