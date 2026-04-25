# v2.8.2 — engine-consolidation

**Date:** 2026-04-25
**Commits:** 10 (on `chore/engine-consolidation`)
**Issue:** [#95](https://github.com/Hekbas/Luth/issues/95)

---

## Overview

Audit-driven housekeeping pass before resuming feature work. Started as a request to "organize and resolve before continuing" — a list of accumulated concerns about doc bloat, missing arch coverage, code-comment hygiene, and untracked allocations. After investigation, several of the worries turned out to be misperceptions (Vulkan validation layers were already correctly implemented; Tracy was integrated, just under-instrumented; V1-V6 markers were meaningful, just opaque to outside readers). Other concerns were real and acted on.

The epic landed seven structural improvements:

1. **ROADMAP** rebuilt with terse summaries, an Effort scale (S/M/L/XL = scope/difficulty, not calendar time) replacing the misleading "Est. Time" column, and the chronologically-correct ordering of v2.7 and v2.8 rows.
2. **Four new arch sub-docs** under `docs/development/arch/` covering systems that existed in code but were undocumented: `memory.md`, `profiling.md`, `validation-layers.md`, `version-glossary.md`.
3. **Comment-banner sanitization** across 14 files in jobs/, memory/, and renderer/, normalizing to the v2.7.0 `// ── Section ──` convention. Net –104 lines of comment cruft.
4. **Tracy global memory hooks** wired via `operator new` / `operator delete` overrides — STL containers and third-party libs are now visible in Tracy's Memory tab (capture-time deep view), complementing `MemoryTracker`'s engine-side runtime stats. The "STL gap" on the in-engine tracker is documented and intentional.
5. **Tracy CPU coverage** filled for editor panels, RenderGraph individual passes, picking readback, and shader hot-reload (gaps explicitly listed in `arch/profiling.md`).
6. **BACKLOG** refreshed: stale v1.5–v1.9 execution order dropped, shipped epics marked with ✓, dependency graph rebuilt around current state, new Epic sections for `frame-debugger-polish`, `animation-quick-pass`, `animation-controller-v2`.
7. **Renames**: `frame-debugger-scrub` → `frame-debugger-polish` (#92), `animation-assets` → `animation-quick-pass` (#93, rescoped to preview UX + rig/clip decoupling). `animation-controller-v2` (#94) inherits the state-machine + blend-tree work.

No code-correctness bugs were uncovered during the sweep; the build stays Debug x64 clean across every commit. Vulkan validation runs without new warnings.

---

## Sub-Tasks

| # | Sub-task | Commit |
|---|---|---|
| A | ROADMAP — Effort scale, terse summaries, reorder, renames | [`44ea3da`](../../../../commit/44ea3da) |
| B | `arch/memory.md` — allocators, tracker categories, STL gap | [`da1073c`](../../../../commit/da1073c) |
| C | `arch/profiling.md` — Tracy primary, GPUTimerPool complementary | [`c7850c4`](../../../../commit/c7850c4) |
| D | `arch/validation-layers.md` — document existing VulkanContext impl | [`716b73c`](../../../../commit/716b73c) |
| E | `arch/version-glossary.md` — V1-V6 markers, two namespaces | [`da04038`](../../../../commit/da04038) |
| F | Comment banners + V1-V6 cross-refs (14 files) | [`b0e358d`](../../../../commit/b0e358d) |
| G | Tracy memory hooks for STL/heap (`GlobalNewDelete.cpp`) | [`e7249fa`](../../../../commit/e7249fa) |
| H | Tracy CPU coverage gaps (10 panels + RG passes + picking + shader reload) | [`8b508e5`](../../../../commit/8b508e5) |
| I | BACKLOG refresh + epic entries | [`dbb5a76`](../../../../commit/dbb5a76) |
| J | History file + version bump | this commit |

---

## Strategic decisions (locked in)

| Concern | Decision | Rationale |
|---------|----------|-----------|
| STL allocation tracking | Accept gap on in-engine `MemoryTracker` (engine-boundary stats by category) + Tracy global hooks for capture-time visibility | Solid coverage without LH::Vector<T> refactor. Override-and-categorize would produce noise; tracked STL allocators is multi-week work. |
| CPU profiling tooling | Tracy only. Expand coverage. No in-engine duplicate. | `LH_PROFILE_*` macros already abstract Tracy. Tracy beats any in-engine UI on flame graphs, fiber visualization, GPU correlation. The "not widely implemented" concern was a coverage problem, not a tooling problem. |
| V1-V6 source comments | Keep markers + add `arch/version-glossary.md` glossary + standardized cross-ref form | Markers are meaningful (asset format versions, JobSystem hazard mitigations). Glossary makes them discoverable to outside readers in one click. |
| Vulkan validation layers | Document only — existing `VulkanContext` impl is correct | Suspected broken; audit confirmed `LUTH_ENABLE_VALIDATION` override + `_DEBUG` autoselect + severity-filtered callback + layer availability gate all correctly wired. |
| Effort metric | S/M/L/XL = scope/difficulty, not calendar time | "We rush 2-3 week stuff in hours sometimes" — calendar estimates were misleading. Scope-based effort survives velocity variance. |
| Epic shape | Single epic, v2.8.2 | Mirrors `editor-cleanup` (v2.7.0) precedent. Splitting into docs+code epics was artificial — arch docs reference comment style and source policy. |

---

## Key Changes

### Documentation

- **ROADMAP completed-epics table** — every summary cell trimmed to ≤2 sentences; v2.7.3-5 reordered to chronologically precede v2.8.0-1; broken local-plan reference dropped (per the "no private paths in GitHub artifacts" rule).
- **ROADMAP planned-epics table** — `Est. Time` replaced with `Effort` (S=hours, M=½–1 day, L=2–5 days, XL=multi-week, but capturing scope/difficulty not calendar). Reorder: `engine-consolidation → frame-debugger-polish → animation-quick-pass → jolt-physics → jiggle-bones → forward-plus → fxaa-taa → animation-controller-v2 → gpu-particles`.
- **`arch/memory.md`** — allocator inventory (`LinearAllocator` page-based bump, `TaggedPageAllocator` 2 MB pages with tag-bulk-free), `MemoryTracker` 9-category atomic counters, opt-in via `LH_NEW`/`LH_ALLOC` macros, V6 hazard cross-ref to fiber-system.md, explicit STL-gap rationale + coverage matrix.
- **`arch/profiling.md`** — Tracy as primary. Build-config gating table (Debug/Release on, Dist off). `LH_PROFILE_*` macro reference. Current coverage list and the gap list filled in this epic. `GPUTimerPool` documented as complementary (per-pass GPU timing, 2-frame latency, Frame Debugger consumer).
- **`arch/validation-layers.md`** — `LUTH_ENABLE_VALIDATION` override matrix, `_DEBUG` autoselect path, `VK_LAYER_KHRONOS_validation` availability gate, severity → engine log level mapping in `DebugCallback`. Closes the "I think it's broken" worry.
- **`arch/version-glossary.md`** — disambiguates the two namespaces sharing the `Vn` prefix: JobSystem hazards (V1-V6 from fiber-system.md) and asset format versions (model V1/V2, shader V1/V2). Indexes all 39 source sites with file:line refs and standardizes the inline comment form.
- **BACKLOG refresh** — Architecture Snapshot pin v2.0.0 → v2.8.2; shipped epics marked with ✅; dependency graph rebuilt; obsolete v1.5–v1.9 execution order dropped; new Epic sections for `engine-consolidation`, `frame-debugger-polish`, `animation-quick-pass`, `animation-controller-v2`. Future-work table extended with "Auto-wire `GPUTimerPool` into RenderGraph" and "Tracked STL allocators (`LH::Vector<T>`)".

### Source

- **Comment banners** — 14 files swept: `jobs/{JobSystem.{h,cpp}, MPMCQueue.h, AtomicCounter.h, IOThread.h, WorkStealingDeque.h, SpinLock.h}`, `memory/{MemoryTracker.h, MemoryMacros.h, TaggedPageAllocator.h}`, `core/FrameData.h`, `renderer/rendergraph/RenderGraph.h`, `renderer/material/MaterialSystem.h`, `renderer/backend/vulkan/VulkanDescriptors.cpp`. Triple-line `// =========` and `// ---------` banners replaced with single `// ── Section ──` lines (or removed where the section is obvious from following declarations). Net –104 lines.
- **V1-V6 cross-refs** — `JobSystem.h` and `JobSystem.cpp` get a file-level `// V<n> markers refer to JobSystem hazards — see arch/version-glossary.md` near the top. `MPMCQueue.h` and `SpinLock.h` get inline cross-refs on the relevant Vn callouts. Inline `// V3:` / `// V5:` / etc. markers are kept terse — the file-level glossary ref is enough disambiguation for readers.
- **`luth/source/luth/memory/GlobalNewDelete.cpp`** — new file. Overrides global `operator new` / `new[]` / `delete` / `delete[]` (plus nothrow + sized variants) to call `TracyAlloc` / `TracyFree`. `#if defined(TRACY_ENABLE)` gate so Dist builds compile to default new/delete with zero overhead. Captures STL containers and third-party libs (Assimp, ImGui, GLFW, VMA) in Tracy's Memory tab.
- **`MemoryMacros.h`** — `LH_NEW`/`LH_ALLOC`/`LH_NEW_ARRAY` and matching free macros drop the redundant `LH_PROFILE_ALLOC`/`LH_PROFILE_FREE` calls. The global override now records each underlying `new`/`delete` once; engine-side `MemoryTracker::RecordAlloc/Free` keeps the per-category atomic counter as before.
- **Tracy CPU coverage** — 10 editor panels (HierarchyPanel, InspectorPanel, ScenePanel, ProjectPanel, ResourcePanel, HistoryPanel, RenderPanel, FrameDebuggerPanel, ProfilerPanel, GamePanel) get `LH_PROFILE_FUNCTION()` at `OnRender` entry. `RenderGraph::Execute`'s pass loop gets `LH_PROFILE_SCOPE_DYNAMIC(pass.name)` so per-pass zones appear in the flame graph (was previously only the top-level `Execute` zone). `PickingSystem::Update` gets a function zone for the GPU readback path. `ShaderWatcher::Poll` and `ShaderLibrary::Reload` get function zones for the hot-reload path.

### Issue housekeeping

- **#92** retitled `refactor: frame-debugger-scrub` → `refactor: frame-debugger-polish`.
- **#93** retitled `refactor: animation-assets` → `refactor: animation-quick-pass` and rescoped (per the user's `animation #2` discussion: preview-toggle UX + decouple rig from clip — small mechanical change). The state-machine + blend-tree work moved to `animation-controller-v2` (#94).

---

## Build verification

Each of the 10 sub-task commits builds Debug x64 clean (only pre-existing warnings: `getenv` deprecation in ProjectLauncher, `strncpy` in InspectorPanel, `chrono::system_clock::rep` cast in Editor, three `LNK4006 NULL_IMPORT_DESCRIPTOR` warnings between linked libs). No new validation errors when running `Luthien.exe` against a scene with assets.

The Tracy memory hooks were verified to compile in Debug, Release, and (with hooks compiled out) Dist via the `TRACY_ENABLE` guard.

---

## Known limitations / future work

- **STL allocator gap (in-engine tracker)** — `MemoryTracker` still doesn't see STL allocations. Tracy covers them at capture time, which is the pragmatic answer for v2.8.x. A future epic introducing tracked STL allocators (`LH::Vector<T>` etc.) would close this if profiling shows it matters.
- **`GPUTimerPool` auto-wiring** — still requires manual `WriteTimestamp` calls per pass. Auto-wire into RenderGraph is candidate work for `frame-debugger-polish` (v2.8.3).
- **Editor panel inner instrumentation** — only `OnRender` entry points are instrumented. Fine-grained zones inside heavy panels (e.g. ProjectPanel's recursive folder walk, InspectorPanel's component drawer dispatch) can be added on demand.
- **Comment sweep coverage** — high-traffic files cleaned (jobs/, memory/, RenderGraph.h, FrameData.h). Lower-priority files (passes/*.cpp, AssetManager.cpp, AssetDatabase.cpp) retain banner comments and can be normalized opportunistically in future commits.

---

## Related docs

- [`arch/memory.md`](../../arch/memory.md) — allocator policy
- [`arch/profiling.md`](../../arch/profiling.md) — Tracy + GPUTimerPool roles
- [`arch/validation-layers.md`](../../arch/validation-layers.md) — Vulkan layer config
- [`arch/version-glossary.md`](../../arch/version-glossary.md) — V1-V6 markers
