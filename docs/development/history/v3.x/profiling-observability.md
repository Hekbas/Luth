# profiling-observability (v3.4.0)

**Date:** 2026-06-20 · **Issue:** [#162](https://github.com/Hekbas/Luth/issues/162)

Comprehensive Tracy coverage + in-editor GPU depth, adapting patterns from a peer engine (Lumina) to Luth's primitives. The scheduler advisor — the one piece that instruments the V1–V6 fiber hot paths — was deferred to [#163](https://github.com/Hekbas/Luth/issues/163) for a dedicated, research-first effort.

## What shipped

**Tracy coverage (the priority)**
- Macro layer: `LH_PROFILE_PLOT` / `_PLOT_CONFIG` / `_MESSAGE(_COLOR)` in `Profiler.h`; GPU-zone macros in new `GpuTracy.h` (keeps `vulkan.h` + `TracyVulkan.hpp` out of the core header).
- GPU zones: per-queue `TracyVkContext` (graphics + async-compute, plain context) on `VulkanContext`; per-pass `TracyVkZoneTransient` in `RenderGraph::Execute`; `TracyVkCollect` per view in `Renderer::EndPrimaryCmdAndSubmit`. The whole RT/compute pipeline is now CPU-correlated on Tracy's GPU timeline.
- Per-frame `TracyPlot` (scheduler health + memory categories + GPU memory) from `App::Run`; `TracyMessage` on shader reload / swapchain recreate / device-lost.
- CPU zone depth: ~300 new zones to peer density across render orchestration, all subsystems, per-pass recording (the recording job is named per pass → legible in the fiber view), asset pipeline, animation, material, shader, backend. Rule held: hot per-element loops get ONE zone, never per-iteration.

**GPU depth (in-editor)**
- Pipeline statistics: `VK_QUERY_TYPE_PIPELINE_STATISTICS` pool in `GPUTimerPool`, graphics passes only, spanning secondaries via `inheritedQueries`. Profiler panel shows per-pass overdraw (FS ÷ target px), color-coded.
- Barrier inspector: `RenderGraph::CaptureBarrierRecords` reads the solver's per-pass barriers from the compiled graph. Profiler panel barrier table with before→after, reason, redundancy filter.

## Key design decisions

- **One Tracy context per queue, plain (not calibrated).** The constructor self-calibrates via a transient cmd buffer, so no `VK_EXT_calibrated_timestamps` dependency.
- **Extend `GPUTimerPool`, not a parallel profiler.** Pipeline stats compose with the existing timestamp pool + `RenderGraphSnapshot`.
- **Barriers captured at compile, not at command emission.** Luth's RG solves barriers as per-pass data, so the inspector is a read of the compiled graph — cleaner than peer engines that intercept the RHI command stream. An architectural payoff of the declarative RG.
- **Per-pass overdraw, not a frame total.** Total FS invocations conflate shadow / half-res / fullscreen passes; per-pass FS ÷ that pass's own target pixels is the correct, actionable overdraw.
- **Heavy capture runtime-toggled, off by default.** Pipeline stats + barrier capture gate on atomic toggles (no CVar system introduced); Tracy CPU/GPU zones stay build-define-gated.

## Bugs fixed along the way

- **GPU timer pool cap too small.** `Init(64)` predated the RT pipeline's growth to ~71 passes; the `passCount > maxPasses` guard silently disabled both per-pass GPU *timing* and the new stats. Raised to 256 + a one-time overflow warning in `ReadResults`. (Found via a runtime diagnostic during smoke testing.)
- **Tracy scope collision.** Two `LH_PROFILE_SCOPE` in one C++ scope redefine `___tracy_scoped_zone`; phase scopes must each get their own braces (bit `ModelImporter`).

## Files

- Macros: `core/diagnostics/Profiler.h`, `renderer/backend/vulkan/GpuTracy.h` (new)
- GPU: `backend/vulkan/{VulkanContext,GPUTimerPool,RenderPassJob}.*`, `Renderer.cpp`, `rendergraph/{RenderGraph,RenderGraphSnapshot}.*`, `RenderPipeline.cpp`
- Plots/messages: `core/App.cpp`, `shader/ShaderWatcher.cpp`, `backend/vulkan/VulkanSwapchain.cpp`
- CPU coverage: ~50 files across `renderer/subsystems/`, `resources/`, `scene/systems/`, `renderer/{material,shader,lighting,backend}/`
- Editor: `luthien/panels/ProfilerPanel.{h,cpp}`

## Verification

Built Debug x64 after each sub-task. Tracy-attached smoke confirmed CPU + GPU zones (graphics *and* async-compute) on the timeline, per-frame plots, and event messages. Pipeline-stats toggle → per-pass overdraw populates (no validation errors); barrier toggle → table lists transitions with reasons + redundancy filter. All user-verified.

## Deferred

Scheduler advisor → [#163](https://github.com/Hekbas/Luth/issues/163) — research-first, touches V1–V6 hot paths.
