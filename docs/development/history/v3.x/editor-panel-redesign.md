# editor-panel-redesign

**Date:** 2026-06-21
**Commits:** 5 on `feat/editor-panel-redesign` (`bd812276` … `8bae327d`)
**Issue:** [#165](https://github.com/Hekbas/Luth/issues/165)
**Series:** standalone effort, Mode B — MINOR bump **v3.5.0 → v3.6.0**, one history file, one `--no-ff` merge, one tag (tag-only; folds into the next milestone Release).

---

## Overview

The editor's two most settings-heavy panels had accumulated UI debt. The **Render panel** was 21 flat
`CollapsingHeader`s under a vestigial 2-tab toggle (tab 0 a permanent "Coming Soon" stub), three of whose headers
(Environment Lighting, Selection Outline, Editor Grid) were **fully duplicated** in the Preferences window. The
**Profiler panel** was 7 flat sections with no pinned overview, mojibake banner comments, only aggregate frame
metrics, and a per-worker swimlane that showed a meaningless per-frame *peak state* rather than occupancy. The
**Scene debug picker** was a flat 12-entry radio popup with a fragile contiguous-enum coupling, at its ceiling.

What shipped: the Render panel regrouped into searchable pipeline-stage tabs; the Scene debug picker rebuilt as a
searchable, categorized picker with seven new debug `ShadeMode`s; and — the bulk of the effort — the Profiler
rebuilt from the ground up as a **scheduler / memory / GPU dashboard** backed by new engine instrumentation
(true per-worker occupancy, GPU memory classified by resource type). Three reusable widget files
(`FilterBox`, `CategoryPopup`, `Charts`) were extracted along the way.

The editor↔engine settings split is preserved throughout: no `EditorSettings` / `RenderingSystem` field changed
ownership, and the new engine instrumentation lives behind the existing `Luth.lib` API (zero new editor includes).

---

## Work areas

| Area | What landed | Commits |
|---|---|---|
| **Render panel** | 21 flat headers → **5 pipeline-stage tabs** (Lighting / Denoise / Environment / Post FX / Path Tracing) + a section search. 4 SVGF instances collapsed into one Denoisers sub-tab (surfacing the unexposed `GetSvgfDiSpecSettings()`); surfaced hidden `gtao.halfRes` + `svgf.alphaMoments`; ⚠ realloc badges; restored IBL/Skybox under Environment; deleted the 3 Preferences-duplicated headers + the dead Model-Viewer tab. Path-Trace convergence rejoined its settings; the Raster/Path-Trace toggle moved out to the Scene toolbar. | `20d8ef94` |
| **Scene debug** | Flat radio popup → searchable, categorized `CategoryPopup` (data-driven `ModeEntry` table with per-entry gates + reason tooltips, killing the contiguous-enum assumption). **7 new `ShadeMode`s** — Metallic / Occlusion / Shadow Cascades (in-shader) and Ambient Occlusion / GI-Raw / DI-Raw / RT-Reflection-Raw (in-shader overrides sampling the textures the lit pass already binds). A Path-Trace (atom) toggle joined the render-mode row, greying the Debug split while active. | `bd812276` |
| **Profiler** | Pinned **overview** (CPU/GPU budget bars with digit-stable widths, CPU/GPU-bound badge, 1%-low + p95/p99, render stats, Tracy pill; colors follow the FPS target). **CPU** scheduler dashboard: throughput / occupancy / steal-efficiency cards, a 2-column per-worker occupancy chart (hover for jobs/steals), a fiber-pool gauge, queue peaks, and the Game/Render stage split. **Memory**: system (CPU) vs GPU split, GPU classified by resource type, filled history area graph. **GPU**: metric cards + one consolidated EMA-smoothed per-pass table (ms bar + overdraw chip) + barriers + Slang-parity pill. | `20d8ef94` `86db53ac` |
| **Engine instrumentation** | `JobSystem`: per-worker **time-in-state occupancy** (nanos banked on each `SetWorkerState` transition — owner-only, lock-free), plus per-worker jobs / steals / deque-depth in `Stats`; the obsoleted per-frame peak-state path removed once the swimlane was gone. `VulkanAllocator`: every allocation **auto-classified by resource type** from its usage flags, tagged into VMA `pUserData` so frees decrement the right bucket. | `6c71751b` `8bae327d` |
| **Widgets** | `FilterBox` (+`PassesFilter`), `CategoryPopup` (filterable grouped radios), and `Charts` (bold `SectionHeader`, `MetricCard`, `StatBar`/`StackedBar`, filled `AreaGraph`, `LegendItem`); `SegmentedButton` gained a full-width tab-strip mode. | `bd812276` `86db53ac` |

---

## Key decisions & deviations

- **Occupancy over swimlanes.** Both the old "cores light up" and the swimlane showed an instantaneous/peak
  state — noise. The CPU tab now shows **time-in-state fractions** (running/stealing/idle), accumulated in the
  engine on each state transition, so saturation + load-balance + steal-waste read at a glance.
- **GPU memory auto-classified, not hand-tagged.** The class is inferred from Vulkan usage flags at the single
  `AllocateBuffer`/`AllocateImage` choke-points (depth/non-uploaded attachment → render target, sampled →
  texture, vertex/index → mesh, accel-structure-storage → BLAS/TLAS, …) and stamped into `pUserData` for a
  symmetric free-time decrement — minimal touch points, no call-site churn.
- **Grouped the Render panel by pipeline stage** (one axis: light → denoise → world → finish → reference), so
  GTAO sits with the lighting it modulates and the Path-Trace toggle lives in the Scene "view mode" row.
- **"Base Color" dropped for "Occlusion"** — Base Color would render pixel-identical to the existing Unlit mode;
  material Occlusion is a distinct, previously-unsurfaced channel.
- **DiReservoir + Overdraw debug modes deferred.** The other engine debug views are in-shader overrides (the
  textures are already bound); these two need new GPU passes (a DI reservoir-viz pipeline / an accumulation pass)
  worth runtime-validating before shipping. An overdraw proxy already exists in the Profiler GPU tab.
- **Quiescent queue metrics shown as peaks.** Fiber-pool and queue depths drain to ~0 between 10 Hz samples, so
  the fiber gauge leads with the engine's true high-water `peak` and queues hold a decaying recent peak; per-pass
  GPU times are EMA-smoothed so the sort order stops jittering frame-to-frame.
- **Faux-bold headers** — the editor ships no bold font, so `BoldText` double-draws with a sub-pixel offset.

---

## Files

- **New widgets:** `luthien/source/luthien/widgets/{FilterBox,CategoryPopup,Charts}.{h,cpp}` (+ `Widgets.h`, `ButtonGroup` full-width)
- **Panels:** `RenderPanel.{h,cpp}` (full rework), `ProfilerPanel.{h,cpp}` (full rework), `ScenePanel.{h,cpp}` (data-driven picker + Path-Trace toggle)
- **Persistence:** `EditorSettings.{h,cpp}` (`renderPanelTab`, `renderDenoiserTab`)
- **Engine:** `luth/source/luth/jobs/JobSystem.{h,cpp}` (per-worker occupancy + counters), `luth/source/luth/renderer/backend/vulkan/VulkanAllocator.{h,cpp}` (GPU-mem-by-type), `luth/source/luth/scene/systems/RenderingSystem.h` (7 `ShadeMode`s + `static_assert`), `luth/assets/shaders/common/pbr_shade.slang` (in-shader debug overrides)

## Verification

- MSBuild Debug x64: clean. `slangc` offline-compile of `pbr.slang` (engine session flags): clean.
- Runtime editor review (user, iterated): Render tabs + search + IBL/Sky; Scene picker + new modes + Path-Trace
  toggle; Profiler overview (digit-stable bars, bound badge, percentiles), CPU occupancy grid + hover, fiber
  gauge, Memory CPU/GPU-by-type, GPU EMA-stable per-pass table.

## Follow-ups

- `DiReservoir` + `Overdraw` debug `ShadeMode`s (need new viz passes).
- True per-frame queue-depth peak tracking in the scheduler (the editor currently approximates with a decaying max).
- Profiler `OnGather`/`ProfilerSnapshot` migration of the global stat reads (still 10 Hz-cached in `OnDraw`).
