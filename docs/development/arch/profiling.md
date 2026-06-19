# Profiling — Tracy (CPU) + GPUTimerPool (GPU)

## Strategy

| Concern | Tool |
|---------|------|
| CPU profiling (live + deep capture) | Tracy — CPU zones at peer density, per-frame plots, event messages |
| GPU per-pass timing | `GPUTimerPool` timestamps (Frame Debugger + Profiler panels) |
| GPU↔CPU correlation | Tracy GPU zones — `TracyVkContext`, per-queue (graphics + async-compute) |
| GPU pipeline statistics | `GPUTimerPool` pipeline-stats pool → Profiler panel per-pass overdraw |
| Render-graph barrier inspection | Solved-barrier capture → Profiler panel barrier table |
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
| `LH_PROFILE_PLOT(name, val)` | `TracyPlot` | Numeric per-frame plot (scheduler / memory counters) |
| `LH_PROFILE_PLOT_CONFIG(name, fmt, …)` | `TracyPlotConfig` | Plot format (bytes / percentage) |
| `LH_PROFILE_MESSAGE(txt)` / `_COLOR` | `TracyMessage(C)` | Discrete event (hot-reload, swapchain recreate, device-lost) |
| `LH_PROFILE_GPU_*` (`GpuTracy.h`) | `TracyVk*` | GPU context / per-pass zone / per-frame collect |

---

## CPU coverage

Comprehensive (peer density, `profiling-observability`). Every non-trivial per-frame / per-asset function is zoned across: render orchestration (`RenderPipeline` + all `subsystems/` + per-pass recording — the recording job is named per pass, so it reads cleanly in the fiber timeline), ECS systems, asset pipeline (importers, Slang compile/reflect), animation, material, shader, and the Vulkan backend. Per-frame `LH_PROFILE_PLOT` counters (scheduler health + memory categories + GPU memory) and `LH_PROFILE_MESSAGE` events emit from `App::Run` and their event sites.

The earlier "known gaps" (editor panels, per-pass record, async I/O, hot-reload, picking readback) are all filled.

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

### GPU Tracy zones

Per-pass GPU zones via `TracyVkContext` — one context per queue (graphics + async-compute; transfer skipped), created in `VulkanContext`, collected per frame in `Renderer::EndPrimaryCmdAndSubmit`. Macros live in `renderer/backend/vulkan/GpuTracy.h`. The whole RT/compute pipeline shows on Tracy's GPU timeline, CPU-correlated.

### GPU pipeline statistics

`GPUTimerPool` also owns a `VK_QUERY_TYPE_PIPELINE_STATISTICS` pool — **graphics passes only** (async-compute queues can't run graphics stat queries). The per-pass query spans the secondary command buffer via `inheritedQueries` (enabled at device creation when supported; degrades to timing-only otherwise). Surfaced in the Profiler panel as per-pass **overdraw** (FS invocations ÷ target pixels), color-coded. Runtime-toggled (`GPUTimerPool::SetStatsEnabled`), off by default. `Init(maxPasses)` must exceed the compiled pass count — `ReadResults` warns once if exceeded.

### Barrier inspector

`RenderGraph::CaptureBarrierRecords` reads the solver's per-pass barriers straight from the compiled graph — no command-stream interception, since Luth's RG solves barriers as data (`before`/`after`/`reason` per pass). Surfaced in the Profiler panel: resource, before→after, reason (RAW/WAW/FINAL), redundant (`before==after`) filter. Runtime-toggled (`RG::RenderGraph::SetBarrierCapture`), off by default.

### Scheduler advisor — deferred

A fiber-scheduler advisor (per-worker timeline, fiber grid, contention/affinity/starvation verdict) is the natural next step, but it instruments the V1–V6 job-system hot paths — deferred to its own researched effort ([#163](https://github.com/Hekbas/Luth/issues/163)). See [fiber-system.md](fiber-system.md).

---

## Connecting to Tracy

Run `Luthien.exe` (Debug or Release build) and attach `Tracy.exe` from `luth/extern/source/tracy/profiler/build/`. The capture starts immediately. Frame markers, zones, and memory allocations populate live.

For deep memory analysis (STL allocations, third-party libs), Tracy's Memory tab catches everything via the global `operator new`/`delete` hooks (see [memory.md](memory.md)).
