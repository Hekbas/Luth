# v2.9.12 — render-pipeline-subsystems

**Date:** 2026-05-06
**Issue:** [#121](https://github.com/Hekbas/Luth/issues/121)
**Branch:** `refactor/render-pipeline-subsystems`
**Mode:** B (per-effort tag, tag-only — internal, no Release)
**Estimate:** M

---

## Overview

Structural refactor of the renderer. Six per-domain subsystems extracted from the `RenderPipeline` god-class; each owns one `.h/.cpp` pair with full lifecycle (state + descriptor set layouts + per-frame uploads + render-graph passes). Friend coupling between `RenderPipeline` ↔ `RenderingSystem` and `RenderPipeline` ↔ `FrameDebuggerContext` fully removed. Zero behavior change — every existing pass, descriptor write pattern, and shader-reload flow preserved verbatim.

The motivation traces back to the v2.9.11 `render-hardening` audit, which spent multi-day effort tracing per-frame UBO descriptor lifetimes that fragmented across 3-5 files per descriptor Set. Reading the `RenderPipeline.h` declaration told you nothing about where any of its ~54 methods were defined — they were spread across 18 different `.cpp` files. The follow-up effort (`fix/per-frame-descriptor-set-cycling`) needs each Set's lifecycle in one file so the change is "swap `VkDescriptorSet x` for `VkDescriptorSet x[MAX_FRAMES_IN_FLIGHT]` in one place." This refactor makes that follow-up a per-subsystem-file edit instead of a scattered hunt.

---

## Sub-Tasks (commit log)

| # | Commit | Subject | Notes |
|---|--------|---------|-------|
| A | `174edbb` | refactor(renderer): drop RenderPipeline friend coupling | Add `RenderingSystem` accessors (`GetFrameDebugger`, `GetFrameAllocator`, `GetDrawList`, `GetCameraParams`); sweep ~169 `m_System.m_X` private reads → public getters; add temp `RenderPipeline` getters for state subsystems will own; drop both `friend` lines on `RenderingSystem.h`; drop `friend FrameDebuggerContext` on `RenderPipeline.h`. 18 files, +223/-201. |
| B | `eeb70e8` | refactor(renderer): extract Global + Lighting subsystems | `GlobalSubsystem` (Set 0 layout + UpdateUBO + WriteView). `LightingSubsystem` (Set 3 + shadow + IBL + skybox + UploadLightUBO). Subsystems own their SPVs + `OnShaderReloaded`. Delete `ShadowInit.cpp`, `IBLInit.cpp`, `ShadowPass.cpp`, `SkyboxPass.cpp`, `GlobalUniforms.cpp`. RP forwards `UpdateGlobalUniforms` / `UploadLightUBO` / `ReloadSkybox`. 18 files, +1094/-909. |
| C | `46873af` | refactor(renderer): extract Geometry subsystem | `GeometrySubsystem` (Set 5 + cull + PBR + DepthPrepass + `BuildGPUObjectBuffer` + `EnsureMaterialRegistered` + entity↔SSBO maps). Delete `GPUObjectBuffers.cpp`, `CullPass.cpp/.h`, `DepthPrepass.cpp`, `GeometryPass.cpp`. 12 files, +1074/-1121. |
| D | `9ef4a59` | refactor(renderer): extract GTAO + PostProcess subsystems | `GTAOSubsystem` (3 GTAO compute layouts + `UpdateUBO` keeping the Global b5 + GTAOMain b2 atomic write). `PostProcessSubsystem` (PP layout + bloom + tonemap + `UpdateUBO` rebinding 4 PP sets). Delete `AOInit.cpp`, `AOPass.cpp`, `BloomPass.cpp`, `PostProcessPass.cpp`. `PostProcessInit.cpp` shrinks to `InitOverlayResources` (Outline + Grid layouts; folded into E). 14 files, +1286/-1257. |
| E | `ece0bc0` | refactor(renderer): extract EditorOverlays subsystem | `EditorOverlaysSubsystem` (Selection + Outline + Grid pipelines/layouts/samplers + `WriteOutlineView`/`WriteGridView` + 3 `Add*Pass` + `CollectSelectedHandles`). Delete `SelectionPass.cpp`, `OutlinePass.cpp`, `GridPass.cpp`, `pipeline/PipelineFactory.cpp`, `postprocess/PostProcessInit.cpp`. Friend coupling fully gone. 10 files, +786/-853. |

5 implementation commits + this wrap-up. Each commit ends in build-clean state with Vulkan validation expected to pass; final smoke gate runs against `ece0bc0` before the merge.

---

## Architectural decisions

### Six subsystems, not eight

The plan-mode Phase 2 agents diverged: domain-partitioning agent suggested 8 (separate IBL + Skybox subsystems); data-flow-partitioning agent suggested 6-7. The user's spec listed 6. Settled on 6:

- **IBL folds into `LightingSubsystem`** because IBL textures bind into Set 3-adjacent pipeline layouts and the existing folder placement (`lighting/IBLInit.cpp`) already conceptually grouped them.
- **Skybox folds into `LightingSubsystem`** because it samples the IBL prefiltered cubemap and shares the same lighting-environment domain.
- **ImGui stays as RP residual** — `AddImGuiPass` is one short pass body, not worth its own subsystem; it's the only pass on the orchestrator.

### Cross-set co-batched UBO writes preserved verbatim (load-bearing)

Verified during plan-mode Phase 3 against `resources/GlobalUniforms.cpp:122-143` and `passes/AOInit.cpp:196-216`:

- `UpdateGlobalUniforms` allocates **one** UBO region from `GPUTaggedPageAllocator` and binds it to **both** `globalDescriptorSet[binding 0]` and `gridDescSet[binding 0]` in **one** batched `vkUpdateDescriptorSets` call.
- `UpdateGTAOUBO` does the same: one allocation, one batched write to `globalDescriptorSet[binding 5]` + `gtaoMainDescSet[binding 2]`.

These atomic writers stay together. Splitting them per-binding would double per-frame heap allocations — a behavior change. Therefore:

- `GlobalSubsystem::UpdateUBO` reaches into `vr.gridDescSet` directly (a tolerated cross-subsystem read; documented invariant).
- `GTAOSubsystem::UpdateUBO` reaches into `vr.globalDescriptorSet` directly.

This rules out the "per-binding writer helper" pattern Plan Agent D considered. The simpler rule wins: the writer that *names* the UBO owns the multi-set write.

### Two-phase init for subsystems with cross-domain pipeline deps

Several subsystems' pipelines need the shared 6-layout vector (Sets 0-5), which can't be assembled until every layout-owning subsystem has run `Init`. Solution: each affected subsystem splits Init into two phases:

```
m_Global.Init()                  // creates Set 0 layout
m_Lighting.Init(*this, hdrPath)  // creates Set 3, shadow map, IBL textures (no pipelines yet)
m_Geometry.Init(*this)           // creates Set 5 layout, cull pipeline (no PBR/DepthPrepass yet)
m_GTAO.Init(*this)               // 3 GTAO compute layouts + pipelines (self-contained)
m_PostProcess.Init(*this)        // PP layout + pipelines (self-contained)
m_EditorOverlays.Init(*this)     // Outline + Grid layouts (no Selection pipelines yet)

// Build the shared 6-layout vector now that all layouts exist.
geoLayouts = { Global, Bindless, Material, Lighting, BoneMatrix, Geometry-Set5 };

m_Lighting.BuildPipelines(geoLayouts);        // shadow + skybox
m_Geometry.BuildPipelines(geoLayouts);        // PBR + DepthPrepass
m_EditorOverlays.BuildPipelines(geoLayouts);  // Selection (uses Sets 0-4 only)
```

This invariant is documented in `RenderPipeline::Initialize` body. GTAO + PostProcess subsystems are self-contained (their pipelines only need their own layouts) so they get a single-phase `Init`.

### Each subsystem owns its own SPV blobs

Pattern: `LightingSubsystem` loads `shadowDepth.{vert,frag,_skinned.vert}` via `ShaderLibrary::LoadEngine` in its `Init`. Its `OnShaderReloaded(name, spv, geoLayouts)` hook handles the shadow + skybox SPV update + pipeline rebuild. The RP-side reload callback dispatches: `m_Lighting.OnShaderReloaded(...) || m_Geometry.OnShaderReloaded(...) || ...` — first subsystem to claim the shader handles it.

`fullscreen.vert` is shared between `PostProcess` (bloom + tonemap pipelines) and `EditorOverlays` (outline + grid). Both subsystems load their own copy via the idempotent `ShaderLibrary::LoadEngine` (returns the same backed SPV). On reload, `PostProcessSubsystem.OnShaderReloaded` returns `false` for `fullscreen.vert` so the dispatcher continues to `EditorOverlays`, which returns `true` and rebuilds its outline/grid pipelines. PP pipelines are already rebuilt before PP returned. This mirrors how multiple ECS systems can subscribe to the same `EditorSignal` event — no central registry, each subsystem's hook decides.

### Friend removal mechanics

Sub-task A drops the `RenderingSystem` friends + the `FrameDebuggerContext` friend on `RenderPipeline.h` together — but the `FrameDebuggerContext` constructor still takes only `RenderPipeline&`. Bridge mechanism: temp public accessors on `RenderPipeline` (`GetGeoPipelineManager`, `GetLightDescSet`, etc.) cover what FrameDebuggerContext needs. As each subsystem extracts in B-E, FrameDebuggerContext's calls flip from `m_Pipeline.Get*()` to `m_Pipeline.Get<X>().Get*()`, and the temp RP accessor is removed. By sub-task E, all temp accessors are gone except `GetSystem()` and `GetCurrentViewResources()` (which are permanent — RP owns frame scratch).

The plan-time alternative of "inject 4 subsystem refs into FrameDebuggerContext's ctor" was rejected: it would force three constructor changes (B, C, D add subsystems one at a time) for marginal gain. The accessor-chain pattern is one extra `.GetX()` call per access site, no API churn across sub-tasks.

---

## File rename / move table

| Pattern | Old | New |
|---|---|---|
| Set 0 layout + UpdateUBO + WriteView | `resources/GlobalUniforms.cpp` | `subsystems/GlobalSubsystem.cpp` |
| Set 3 + shadow map + sampler | `lighting/ShadowInit.cpp` | `subsystems/LightingSubsystem.cpp` |
| IBL precompute integration + ReloadSkybox | `lighting/IBLInit.cpp` | `subsystems/LightingSubsystem.cpp` |
| AddShadowPass | `passes/ShadowPass.cpp` | `subsystems/LightingSubsystem.cpp` |
| AddSkyboxPass | `passes/SkyboxPass.cpp` | `subsystems/LightingSubsystem.cpp` |
| **`UploadLightUBO` (naming-mismatch fix)** | `gpu/GPUObjectBuffers.cpp` | `subsystems/LightingSubsystem.cpp` |
| Set 5 + cull + BuildGPUObjectBuffer + EnsureMaterialRegistered | `gpu/GPUObjectBuffers.cpp` | `subsystems/GeometrySubsystem.cpp` |
| AddCullComputePass | `passes/CullPass.{h,cpp}` | `subsystems/GeometrySubsystem.cpp` |
| AddDepthPrepass | `passes/DepthPrepass.cpp` | `subsystems/GeometrySubsystem.cpp` |
| AddGeometryPass | `passes/GeometryPass.cpp` | `subsystems/GeometrySubsystem.cpp` |
| BuildPBRPipelines + BuildDepthPrepassPipelines | `pipeline/PipelineFactory.cpp` | `subsystems/GeometrySubsystem.cpp` |
| BuildShadowPipelines + BuildSkyboxPipeline | `pipeline/PipelineFactory.cpp` | `subsystems/LightingSubsystem.cpp` |
| BuildPostPipelines | `pipeline/PipelineFactory.cpp` | `subsystems/PostProcessSubsystem.cpp` |
| BuildSelectionPipelines + BuildOutlinePipeline + BuildGridPipeline | `pipeline/PipelineFactory.cpp` | `subsystems/EditorOverlaysSubsystem.cpp` |
| InitAOResources + UpdateGTAOUBO | `passes/AOInit.cpp` | `subsystems/GTAOSubsystem.cpp` |
| 3 GTAO Add*Pass | `passes/AOPass.cpp` | `subsystems/GTAOSubsystem.cpp` |
| InitPostProcessResources (PP layout) + UpdatePostProcessUBO | `postprocess/PostProcessInit.cpp` | `subsystems/PostProcessSubsystem.cpp` |
| AddBloomPasses | `passes/BloomPass.cpp` | `subsystems/PostProcessSubsystem.cpp` |
| AddPostProcessPass (renamed AddCompositePass) | `passes/PostProcessPass.cpp` | `subsystems/PostProcessSubsystem.cpp` |
| Outline + Grid layouts/samplers | `postprocess/PostProcessInit.cpp` | `subsystems/EditorOverlaysSubsystem.cpp` |
| AddSelectionMaskPass + CollectSelectedHandles | `passes/SelectionPass.cpp` | `subsystems/EditorOverlaysSubsystem.cpp` |
| AddOutlinePass | `passes/OutlinePass.cpp` | `subsystems/EditorOverlaysSubsystem.cpp` |
| AddGridPass | `passes/GridPass.cpp` | `subsystems/EditorOverlaysSubsystem.cpp` |
| WriteViewGlobalSet | `ViewResources.cpp` | `subsystems/GlobalSubsystem.cpp` (as `WriteView`) |
| WriteViewPostProcessSets | `ViewResources.cpp` | `subsystems/PostProcessSubsystem.cpp` (as `WriteView`) |
| WriteViewGTAOSets | `ViewResources.cpp` | `subsystems/GTAOSubsystem.cpp` (as `WriteView`) |
| WriteViewOutlineSet | `ViewResources.cpp` | `subsystems/EditorOverlaysSubsystem.cpp` (as `WriteOutlineView`) |
| WriteViewGridSet | `ViewResources.cpp` | `subsystems/EditorOverlaysSubsystem.cpp` (as `WriteGridView`) |

**18 files deleted, 12 files created.** The `gpu/` and `postprocess/` folders empty; `pipeline/` keeps `PipelineManager.cpp` + `PipelineCache.cpp` (utilities); `passes/` keeps only `ImGuiPass.cpp` (the lone residual pass).

---

## What stays on RenderPipeline (orchestrator residual)

`RenderPipeline.cpp` shrinks to ~650 LOC:

- **Lifecycle**: `Initialize` (subsystem `Init` fan-out + 6-layout vec + `BuildPipelines` fan-out + `m_GTAO.Init`), `Shutdown` (reverse), `Execute` (orchestrates per-view pass-add by calling subsystem methods), `ExecuteMinimal`, `OnResize`, `PrepareForTargets`.
- **Frame scratch**: `m_CurrentView`, `m_CurrentViewResources`, `m_System` (RenderingSystem ref), `m_Debugger` (FrameDebuggerContext owner). Read by every subsystem's pass-add lambdas via the `m_Pipeline->GetCurrentView()` / `GetCurrentViewResources()` accessors.
- **View resources management**: `m_ViewResources` map, `EnsureViewResources` (public — used by editor `GamePanel`), `ReleaseViewResources`, `AllocateViewResources`, `RecreateViewTextures`, `DestroyViewResources`. The `ViewResources` struct definition still lives in `RenderPipeline.h` since it's the per-view bundle every subsystem reads.
- **`AddImGuiPass`**: stays on RP (single-view residual; only one Add*Pass left).
- **Named textures registry**: `m_NamedTextures`, `RegisterNamedTextures` (queries each subsystem for textures to register), `GetNamedTexture`.
- **Shader hot-reload dispatcher**: `m_ShaderWatcher` + the dispatch path. Each subsystem registers its `OnShaderReloaded` hook; RP's reload callback iterates them.
- **FrameDebugger forwarders**: `ReplayPassUpToDraw`, `BlitArchivedDepthToPreview`, `CaptureSnapshot`, capture-state fan-out from `Execute`, preview accessors.
- **Public-API forwarders**: `UpdateGlobalUniforms`, `UpdatePostProcessUBO`, `UpdateGTAOUBO`, `BuildGPUObjectBuffer`, `UploadLightUBO`, `EnsureMaterialRegistered`, `ReloadSkybox`, `GetMaterialSlotMap`, `GetEntityToSSBOIndex`, `GetEntityLookup` — all forward to the appropriate subsystem. Public callers (`RenderingSystem::Update`, editor) keep working without edits.

`ViewResources.cpp` shrinks similarly to ~165 LOC: `EnsureViewResources` / `ReleaseViewResources` (public), `AllocateViewResources` (orchestrates: create pool → allocate per-subsystem sets → call each subsystem's `WriteView` in order; Global writes last because it reads `vr.gtaoFinal` set up by GTAO's `WriteView`), `RecreateViewTextures`, `DestroyViewResources`. The `MakeGlobalCtx` helper in an anonymous namespace builds the `GlobalViewWriteContext` from RP-side state for the Global subsystem's per-view write.

---

## Architectural alignment

- **Cornerstone 1 (per-frame data via tagged allocators):** unchanged. Every per-frame UBO write — `GlobalSubsystem::UpdateUBO`, `LightingSubsystem::UploadLightUBO`, `GeometrySubsystem::BuildGPUObjectBuffer`, `GTAOSubsystem::UpdateUBO`, `PostProcessSubsystem::UpdateUBO` — routes through `Memory::GPUTaggedPageAllocator::Get()` exactly as before. Tag = render-frame index. No new primitive.
- **Cornerstone 2 (job system):** unchanged. No new sync primitives. Subsystem methods that run in fibers (passes, UBO uploads) use the same `JobSystem::GetCurrentJobContext()` pattern as before.
- **Cornerstone 4 (no `new`/`delete` in render):** unchanged. Subsystem instances are direct members of `RenderPipeline` (not `unique_ptr`); `LH_NEW` / `LH_ALLOC` macros not introduced.
- **Cornerstone 5 (no legacy Vulkan):** unchanged. UPDATE_AFTER_BIND, Timeline Semaphores, sync2 — all preserved verbatim.
- **Cornerstone 6 (editor decoupling):** unchanged. `Luth.lib` has zero `luthien/...` includes. Editor side touches subsystems only via `RenderingSystem::GetPipeline().Get<X>()` (plus the existing public API on `RenderPipeline` which forwards to subsystems).
- **`arch/rendering-pipeline.md` descriptor table:** updated with subsystem-ownership footnote — see commit.

---

## Build verification

Debug x64 builds clean (0 errors, pre-existing warnings only — `LNK4006` from `vulkan-1.lib` symbol overlap with `shaderc_shared.dll`/`ws2_32.dll`/`dbghelp.dll`, `C4244` chrono conversion in `Editor.cpp:619`). Validation layers smoke-test deferred to user's runtime gate (next).

---

## Smoke gate

After the wrap-up commit and before merging to `main`, the user is asked to run a full smoke test (per memory `feedback_smoke_gate_before_merge.md`). This is a structural refactor with zero visible UX change, so the bar is "did anything regress." Visual targets:

- Drag editor camera 60+s in Lit / Unlit / Wireframe — no `VUID-*` errors.
- PostProcess settings toggled during motion — no descriptor mismatch errors.
- Game panel + Scene panel concurrent — shadow map renders without inter-view artifacts (the v2.9.11 fix is preserved).
- Frame Debugger capture (Scene + Game source) — `ReplayPassUpToDraw` shows expected per-draw previews.
- Hot-reload — edit any of `pbr.frag`, `shadowDepth.vert`, `bloomBlur.frag`, `outline.frag` and observe pipeline rebuild without crashes.
- Long-session memory: `GPUTaggedPageAllocator` working set steady-state ~6 MB (matches v2.9.11 baseline).

Once the user gives go-ahead, the merge + tag run as:

```
git checkout main
git merge --no-ff refactor/render-pipeline-subsystems -m "feat(release): merge refactor/render-pipeline-subsystems (#121)"
git tag -a v2.9.12 -m "v2.9.12 — render-pipeline-subsystems"
git push origin main --follow-tags
```

Tag-only — no `gh release create` (Mode B internal — see CLAUDE.md "Tagging vs. releasing").

---

## Outstanding follow-ups

1. **Per-frame descriptor-set cycling** (the next effort this refactor enables). With each Set's full lifecycle now in one subsystem file, swapping `VkDescriptorSet x` for `VkDescriptorSet x[MAX_FRAMES_IN_FLIGHT]` becomes a 6-file edit instead of an 18-file hunt.
2. **`renderer/` folder coherence drive-by**: post-refactor, several legacy folders (`gpu/`, `postprocess/`) are empty. The `passes/` folder holds only `ImGuiPass.cpp`. Cleanup deferred to a future trivial commit (probably alongside the comment-policy refactor).
3. **`backend/` → `vulkan/` folder rename**: discussed during plan-mode but rejected after closer inspection — `RenderBackend` IS a real abstraction (frame-loop level only; resources stay direct `VK*` types), so `backend/vulkan/` is accurate, not aspirational. No change.
