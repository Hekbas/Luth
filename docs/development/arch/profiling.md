# Profiling — Tracy (CPU) + GPUTimerPool (GPU)

## Strategy

| Concern | Tool |
|---------|------|
| CPU profiling (live + deep capture) | Tracy |
| GPU per-pass timing (Frame Debugger panel) | `GPUTimerPool` |
| Memory allocation profiling | Tracy memory zones (see [memory.md](memory.md)) |

**Tracy is the primary profiler.** Fully integrated via the `LH_PROFILE_*` macro layer in [`Profiler.h`](../../../luth/source/luth/core/diagnostics/Profiler.h). The macros are a thin abstraction — swapping profiler backends would touch one file. No in-engine CPU profiler is maintained in parallel; building one would duplicate Tracy's flame-graph UI, fiber visualization, and GPU-CPU correlation for marginal gain.

`GPUTimerPool` is **not** a Tracy duplicate. It captures per-pass GPU timestamps for the in-editor Frame Debugger panel (replay-and-scrub workflow), which Tracy doesn't replace.

---

## Build configuration

Tracy is enabled in **Debug** and **Release**, disabled in **Dist**.

Defines per build config (declared in `luth/premake5.lua`, `luthien/premake5.lua`, `runtime/premake5.lua`, `luth/extern/premake5-tracy.lua`):

| Config | Defines | Tracy state |
|--------|---------|-------------|
| Debug   | `DEBUG`, `TRACY_ENABLE`, `TRACY_FIBERS` | Active — full instrumentation, fiber name tracking |
| Release | `RELEASE`, `TRACY_ENABLE`, `TRACY_FIBERS` | Active — same as Debug |
| Dist    | (no Tracy defines) | Compiled out — `TracyClient.cpp` builds an empty lib; all `LH_PROFILE_*` macros expand to no-ops |

When `TRACY_ENABLE` is undefined, every `LH_PROFILE_*` macro is `#define`d to nothing — zero runtime cost, zero binary footprint.

---

## Macro reference

Defined in [`luth/source/luth/core/diagnostics/Profiler.h`](../../../luth/source/luth/core/diagnostics/Profiler.h):

| Macro | Tracy primitive | Use |
|-------|-----------------|-----|
| `LH_PROFILE_FRAME(name)` | `FrameMarkNamed` | Mark frame boundaries (one per swapchain present) |
| `LH_PROFILE_FUNCTION()` | `ZoneScoped` | Auto-named scope from `__FUNCTION__` |
| `LH_PROFILE_SCOPE(name)` | `ZoneScopedN(name)` | Static-string named scope |
| `LH_PROFILE_SCOPE_DYNAMIC(name)` | `ZoneScoped` + `ZoneName` | Runtime-string scope (e.g. pass name) |
| `LH_PROFILE_TAG(key, val)` | `ZoneText` | Annotate current zone with a string |
| `LH_PROFILE_ALLOC(ptr, size)` | `TracyAlloc` | Memory allocation event (paired with `LH_NEW`/`LH_ALLOC`) |
| `LH_PROFILE_FREE(ptr)` | `TracyFree` | Memory free event |
| `LH_PROFILE_THREAD(name)` | `tracy::SetThreadName` | Name an OS thread in Tracy |
| `LH_PROFILE_FIBER_ENTER(name)` | `TracyFiberEnter` | Mark fiber resume (paired with leave) |
| `LH_PROFILE_FIBER_LEAVE` | `TracyFiberLeave` | Mark fiber yield |

---

## CPU coverage

Currently instrumented (~18 files):

| Subsystem | Files |
|-----------|-------|
| App / Editor entry | `core/App.cpp`, `Editor.cpp` |
| Job system | `jobs/JobSystem.cpp`, `jobs/IOThread.cpp` |
| ECS systems | `scene/systems/{Transform,Camera,Animation,Lighting,Rendering}System.{h,cpp}` |
| Renderer | `renderer/rendergraph/RenderGraph.cpp` (top-level execute) |
| Vulkan backend | `VulkanContext.cpp`, `VulkanSwapchain.cpp`, `UploadContext.cpp` |
| Asset pipeline | `AssetManager.cpp`, `ModelImporter.cpp`, `TextureImporter.cpp` |
| Memory | `memory/MemoryMacros.h` (alloc/free events) |

### Known gaps (filled in v2.8.2 `engine-consolidation`)

- **Editor panels** — `Update`/`Render` entry points uninstrumented (`HierarchyPanel`, `InspectorPanel`, `ScenePanel`, `ProjectPanel`, `ConsolePanel`, `FrameDebuggerPanel`)
- **RenderGraph individual passes** — only the top-level `RG::Execute` has a zone; per-pass record/execute is invisible in the flame graph
- **Async I/O hot paths** — `IOThread::Submit` and async asset load callbacks need `LH_PROFILE_SCOPE`
- **Material / shader hot-reload** — `ShaderWatcher::OnFileChanged`, Material rebuild path
- **PickingSystem GPU readback** — wait + entity ID resolution

### Style guidance

- Use `LH_PROFILE_FUNCTION()` at the top of any function whose runtime is non-trivial.
- Use `LH_PROFILE_SCOPE("ShortName")` for inner blocks worth distinguishing.
- For per-pass / per-asset zones with runtime-string names, use `LH_PROFILE_SCOPE_DYNAMIC(passNameString)`.
- Don't instrument leaf math helpers — Tracy zone overhead is small but non-zero, and noise crowds the flame graph.
- Always pair `LH_PROFILE_FIBER_ENTER` with `LH_PROFILE_FIBER_LEAVE` on the same fiber's resume/yield boundary.

---

## GPU profiling — `GPUTimerPool`

[`luth/source/luth/renderer/backend/vulkan/GPUTimerPool.h`](../../../luth/source/luth/renderer/backend/vulkan/GPUTimerPool.h) — per-frame `VkQueryPool` of timestamp queries.

```cpp
GPUTimerPool::Init(u32 maxPasses);
GPUTimerPool::ResetForFrame(VkCommandBuffer cmd);
GPUTimerPool::WriteTimestamp(VkCommandBuffer cmd, u32 passIndex, bool isBegin);
GPUTimerPool::ReadResults(u32 passCount, std::vector<float>& outTimesMs);
```

- Two timestamps per pass (begin / end).
- Results read with **2-frame latency** so the queries are guaranteed complete (matches `MAX_FRAMES_IN_FLIGHT = 3`).
- Manual insertion per pass — no automatic render-graph wiring (a future epic, deferred from v2.8.2).
- Consumed by Frame Debugger panel for the per-pass timing column.

GPU work that doesn't go through render-graph passes (compute uploads via `UploadContext`, async transfer queue) is currently not measured by `GPUTimerPool`. Tracy's GPU zones (via `TracyVkContext`) are not yet wired — a candidate for a future profiling-coverage pass.

---

## Connecting to Tracy

Run `Luthien.exe` (Debug or Release build) and attach `Tracy.exe` from `luth/extern/source/tracy/profiler/build/`. The capture starts immediately. Frame markers, zones, and memory allocations populate live.

For deep memory analysis (STL allocations, third-party libs), Tracy's Memory tab catches everything via the global `operator new`/`delete` hooks (see [memory.md](memory.md)).
