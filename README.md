<p align="center">
  <img src="docs/screenshots/luth_logo.png" alt="Luth Logo" width="350"/>
</p>

<p align="center">
  <a href="https://github.com/Hekbas/Luth/releases/latest">
    <img alt="Version" src="https://img.shields.io/github/v/release/Hekbas/Luth?style=for-the-badge&color=007ec6">
  </a>
  <a href="https://github.com/Hekbas/Luth/actions">
    <img alt="Build Status" src="https://img.shields.io/github/actions/workflow/status/Hekbas/Luth/build.yml?style=for-the-badge">
  </a>
  
<br>
  <img alt="Language" src="https://img.shields.io/badge/Language-C++20-2b2d31.svg?style=for-the-badge&logo=c%2B%2B">
  <img alt="Platform" src="https://img.shields.io/badge/Platform-Windows-2b2d31.svg?style=for-the-badge&logo=windows">

  <a href="https://github.com/Hekbas/Luth/blob/main/LICENSE">
    <img alt="License" src="https://img.shields.io/badge/License-MIT-2b2d31.svg?style=for-the-badge">
  </a>
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

It started with my Bachelor's Thesis, where I designed a dual-renderer engine to benchmark Vulkan path tracing against traditional OpenGL PBR. The focus was purely on real-time graphics, so the underlying architecture was single-threaded. It worked, and I had a blast building it!

Then I watched Christian Gyrling’s GDC talk on *[Parallelizing the Naughty Dog Engine Using Fibers](https://www.gdcvault.com/play/1022186/Parallelizing-the-Naughty-Dog-Engine)*. Seeing how they saturated every single CPU core made me realize how much was left to explore.

So, I started Luth from scratch to explore high-performance architecture: fiber-based job systems, lock-free memory models, and bindless Vulkan rendering. It is absolutely over-engineered for a solo project, but that’s the point.

---

## Shuddup! how build??

**Prerequisites:**
- **OS**: Windows 10 / 11
- **Compiler**: MSVC (v143+) or Clang (C++20-compliant)
- **SDK**: [Vulkan SDK 1.3+](https://vulkan.lunarg.com). Needs `dynamicRendering`, `timelineSemaphore`, and descriptor indexing with UBO update-after-bind (any GPU 2018+)

**Steps:**
1.  **Clone with submodules**
    ```bash
    git clone --recursive https://github.com/Hekbas/Luth.git
    ```
2.  **Generate the VS solution**
    ```bash
    scripts/setup/setup_windows.bat
    ```
3.  **Build** — either open `Luth.sln` in Visual Studio 2022, or run the headless script:
    ```bash
    scripts/build/build_windows.bat
    ```

The editor binary lands at `bin/windows-x86_64/Debug/Runtime/Luthien.exe`.

---

## Technical Architecture

### 1. The Fiber Job System
Instead of dedicated OS threads per task ("Render Thread", "Audio Thread"), Luth treats the CPU as a generic worker pool.
* **N:M Threading:** One Worker Thread per CPU core. Logical tasks are wrapped in **Fibers** aka lightweight user-mode stacks that migrate freely between workers.
* **Zero Blocking:** When a job waits on a dependency (or the GPU), it yields to the scheduler, which swaps in another fiber. CPU saturation stays near 100%.
* **Synchronization:** **SpinLocks** (test-and-set + `_mm_pause()`) and **Atomic Counters** keep critical sections short, never blocks the OS.

### 2. Pipelined Frame Execution
Three stages overlap. At any frame `T`, the engine is processing three frames at once:

```
time ──►
              ┌──────────┬──────────┬──────────┬──────────┐
   CPU game   │ N        │ N+1      │ N+2      │ N+3      │
              ├──────────┼──────────┼──────────┼──────────┤
   CPU render │ N-1      │ N        │ N+1      │ N+2      │
              ├──────────┼──────────┼──────────┼──────────┤
   GPU exec   │ N-2      │ N-1      │ N        │ N+1      │
              └──────────┴──────────┴──────────┴──────────┘
```

1. **Game (N):** Transform / animation updates, then captures a `RenderSnapshot` POD into the frame's `LogicMemory` arena — the immutable handoff to the next stage.
2. **Render (N-1):** Reads frame N-1's snapshot, builds the render graph, dispatches per-pass secondary cmd buffer recording in parallel, submits.
3. **GPU (N-2):** Executes the commands submitted previously.

Game and render run concurrently on worker fibers from frame 2 onward (frames 0/1 are a sync warm-up against the current frame). The frame boundary is the snapshot, not shared mutable state — Game writes to one `FrameContext` slot, Render reads from another. Stage-isolated subsystems that retain mutexes (`MaterialSystem`, `BoneMatrixBuffer`) `assert` they're only mutated from the game stage.

### 3. Memory Strategy
`new` / `delete` are forbidden in the hot path. Two allocators handle everything that churns:

```
Page Pool (2 MB virtual pages)
 ├── TaggedPageAllocator      —  CPU side, tagged lifetime, bulk free
 │   └── per-thread cache     —  lock-free hot-path allocations
 ├── GPUTaggedPageAllocator   —  host-mapped device pages, freed when GPU N-2 retires
 │   └── per-frame UBO/SSBO regions, descriptors rebind via UPDATE_AFTER_BIND
 └── LinearAllocator          —  per-frame, reset on Begin()
```

* **Tagged Page Allocator** — Naughty Dog–style. Allocations carry a tag (`LevelGeometry`, `Frame_N`, …) and are freed in bulk by tag.
* **GPU Tagged Page Allocator** — sibling of the CPU side. Vends 2 MB pages from host-mapped device backings; bulk-freed when the GPU N-2 timeline value retires.
* **Linear Allocator** — bump-allocate transient frame data (command lists, UI state); resets each frame, no per-object destructors.

Persistent SSBOs (Material Set 2, Light Set 3, Object Set 5) are triple-buffered so frame N writes never overlap frame N-1 GPU reads.

### 4. Vulkan 1.3 Backend
Modern hardware, minimal driver overhead.
* **Bindless Descriptors:** `VK_EXT_descriptor_indexing` binds all engine textures to one global array (`Set 0`). Materials store an integer index — any draw call can sample any texture without rebinding.
* **Dynamic Rendering:** No `VkRenderPass` / `VkFramebuffer` — passes use `vkCmdBeginRendering` directly.
* **Timeline Semaphores:** Replace `vkWaitForFences`. A dedicated **Poller Job** queries semaphore values and wakes dependent fibers only when the GPU finishes their workload.
* **Update-After-Bind:** Per-frame UBO/SSBO descriptor sets are rewritten each frame as their backing GPU pages cycle, eliminating CPU-GPU sync on those bindings.
* **VMA:** [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) handles all device-memory placement (buffers, images, staging).

### 5. Render Graph
Each frame, Luth builds a **DAG** of render passes. Passes declare reads and writes through a `RenderPassBuilder`; the graph solves pipeline barriers, culls unused passes, and computes resource lifetimes automatically.

```cpp
graph.AddPass<GeometryPassData>("GeometryPass",
    [&](GeometryPassData& data, RG::RenderPassBuilder& builder) {
        data.depthTex  = builder.WriteDepth(sceneDepth, ...);
        data.outputTex = builder.Write(sceneColor);
        data.indirect  = builder.ReadIndirectBuffer(indirectBuffer);
    },
    [=](GeometryPassData& data, RG::RenderPassContext& ctx) {
        // record draw commands on ctx.commandBuffer
    });
```

Passes execute in topological order; command-buffer recording inside each pass parallelizes across worker threads.

---

## Features

### Rendering

| | |
|---|---|
| **PBR** | Cook-Torrance BRDF, metallic/roughness, render-mode variants (Opaque/Cutout/Transparent) |
| **Lighting** | 1 directional + up to 64 point lights, ECS-driven |
| **Shadows** | 4-cascade PSSM, per-cascade GPU cull, PCF, cascade blending |
| **Ambient Occlusion** | GTAO half-res compute (prefilter → integrate → bilateral denoise) |
| **GPU Culling** | Compute frustum cull per cascade + main scene, indirect draws everywhere |
| **IBL** | HDR skybox, diffuse irradiance + pre-filtered specular + BRDF LUT, split-sum ambient |
| **Post-Processing** | HDR pipeline, bloom, 4 tonemap operators, vignette, grain, chromatic aberration |
| **Shaders** | Single-stage SPIR-V asset pipeline with UUIDs, hot-reload, SPIRV-Cross reflection |
| **Pipeline Cache** | Disk-persisted, lazy variant creation, targeted hot-reload invalidation |
| **Mipmaps** | Per-texture pipeline with sampler maxLod control |

### Animation

| | |
|---|---|
| **Sampling** | Fiber-parallel keyframe evaluation |
| **GPU Skinning** | Bone matrix SSBO, vertex shader skinning |
| **Blending** | SQT interpolation, crossfade transitions, layered override with bone masks |
| **Root Motion** | Automatic extraction and application to entity transform |
| **Debug** | Bone overlay visualization in editor viewport |

### Asset Pipeline

| | |
|---|---|
| **Asset Database** | UUID-based registry with `.meta` sidecars, importers for shaders/textures/models/materials/animations |
| **Smart Import** | Multi-strategy texture discovery, drag-and-drop with eager import, texture remap dialog |
| **Hot Reload** | FileWatcher-based live reload for shaders, textures, and project files |
| **Scene Format** | Custom JSON `.luth` format with dirty tracking and native file dialogs |

### Editor

| | |
|---|---|
| **Scene Interaction** | Mouse picking (ID buffer), selection outlines with occluded fade, shade modes (Lit/Wireframe/Unlit) |
| **Inspector** | Material editor, animation controls, light/shadow settings, Add Component workflow |
| **Inspector Preview** | Live orbit-camera 3D preview for Material/Model assets |
| **Play Mode** | Editing/Playing/Paused state machine, JSON scene snapshot, animation gating, transport bar |
| **Game Panel** | Dedicated camera-driven runtime view with letterbox, no overlays |
| **Project Panel** | Folder navigation, search, hot reload, context menus for entity/primitive creation |
| **Thumbnails** | Rendered previews for textures/meshes/materials in Project panel |
| **Undo / Redo** | Command pattern with UUID-based entity resolution, gizmo drag coalescing, compound commands, material snapshot undo |
| **Frame Debugger** | Freeze a frame, scrub through every draw, replay any single one to see what it did |
| **Profiler** | Per-system timing breakdown with fiber-aware instrumentation |
| **Persistence** | Window layouts, editor settings, and panel state saved across sessions |

---

## Roadmap

See the full [development roadmap](docs/development/ROADMAP.md) for completed phases and version history.

### Future Ideas

**Rendering** — Deferred GBuffer, Forward+ clustered lighting, FXAA/TAA, global illumination, volumetric fog, SSR

**Gameplay** — Physics (Jolt, jobified), GPU particle system, animation blend trees & IK, prefab system, scripting (C#/Lua)

**Editor** — Asset streaming, visual shader editor

---

## Dependencies

LUTH Engine is built on the shoulders of giants:

| | |
|---|---|
| [**Vulkan SDK**](https://www.lunarg.com/vulkan-sdk/) | Rendering backend |
| [**VMA**](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | Vulkan memory allocator |
| [**shaderc**](https://github.com/google/shaderc) | Runtime GLSL → SPIR-V compilation (ships with Vulkan SDK) |
| [**SPIRV-Cross**](https://github.com/KhronosGroup/SPIRV-Cross) | Shader reflection |
| [**EnTT**](https://github.com/skypjack/entt) | Entity-Component-System |
| [**ImGui**](https://github.com/ocornut/imgui) | Editor GUI |
| [**ImGuizmo**](https://github.com/CedricGuillemet/ImGuizmo) | Translate / rotate / scale gizmos |
| [**Tracy**](https://github.com/wolfpld/tracy) | Frame profiler |
| [**GLFW**](https://www.glfw.org/) | Windowing + input |
| [**GLM**](https://glm.g-truc.net/) | Math |
| [**spdlog**](https://github.com/gabime/spdlog) | Logging |
| [**assimp**](https://github.com/assimp/assimp) | Model importing |
| [**stb_image**](https://github.com/nothings/stb) | Image loading |
| [**nlohmann/json**](https://github.com/nlohmann/json) | JSON serialization |

**Planned integrations:**
* [**Jolt Physics**](https://github.com/jrouwe/JoltPhysics) — rigid body physics, jobified onto the fiber scheduler

---

## License

Released under the [MIT License](LICENSE).