# Rendering Pipeline — Architecture Details

## Descriptor Set Allocation

| Set | Content | Updated |
|-----|---------|---------|
| 0 | GlobalUniforms + shadow cascade array + IBL irradiance + IBL prefiltered env + BRDF LUT + GTAO settings UBO (6 bindings) | Per frame — cycled across `MAX_FRAMES_IN_FLIGHT` slots |
| 1 | Bindless textures (16384 slots) | On upload-fence retire (UPDATE_AFTER_BIND, partially-bound; deferred via `UploadContext` pump per `texture-async-uploads` v2.8.14) |
| 2 | Material SSBO (16384 entries) | Per game stage — cycled across `MAX_FRAMES_IN_FLIGHT` slots |
| 3 | Light UBO (dir + point lights) + shadow map sampler | Per frame — cycled across `MAX_FRAMES_IN_FLIGHT` slots |
| 4 | `BoneMatrixBuffer` SSBO (per-entity skinning blocks) | Per game stage — cycled across `MAX_FRAMES_IN_FLIGHT` slots |
| 5 | `GPUObjectData` SSBO — per-draw transforms/IDs for indirect dispatch | Per render stage — cycled across `MAX_FRAMES_IN_FLIGHT` slots |

> Set 0 expanded from 4 → 6 bindings across `csm` (v1.3.0 — cascade array) and `gtao` (v1.5.0 — AO sampler + settings UBO). Set 4 added by `animation-gpu-skinning`; Set 5 by `compute-gpu-culling` (v1.2.0). Sets 2/4/5 moved to per-stage rebind in `gpu-tagged-heap` (v2.8.10) — backing storage allocated each frame from `GPUTaggedPageAllocator`, descriptors rewritten via `vkUpdateDescriptorSets`. The cull descriptor (binding into Set 5 + Indirect Buffer for compute) follows the same pattern. Set 1 bindless registration moved from synchronous-in-VKTexture-ctor to a `UploadContext` pending-bind pump in `texture-async-uploads` (v2.8.14) — `VKTexture` ctor pushes `{outIndex, view, sampler, fence}`; pump drains in `AssetManager::Update` once `IsComplete(fence)` and writes the slot through `outIndex`. Until then `m_BindlessIndex == INVALID_BINDLESS_SLOT` and `Material::BindlessOrNull` keeps shaders on reserved white slot 0. `~VKTexture` cancels by view-handle.

> **Per-frame descriptor cycling (`per-frame-descriptor-set-cycling` v2.9.13).** Every set whose binding is rewritten per stage to a fresh tagged-heap region — Sets 0, 2, 3, 4, 5, the cull descriptor, the 4 PostProcess sets, the GTAO main set, and the Grid set — is now `std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>`. Render-stage bind sites index by `Renderer::GetFrameData()->GetRenderFrameIndex() % MAX_FRAMES_IN_FLIGHT`; game-stage `Update()` writes the GAME-frame slot (`GetFrameIndex() % N`). Game frame K writes slot K%N while render stage of frame K-1 reads slot (K-1)%N — distinct slots, race-free, no UAB needed. Set 1 (bindless) keeps UAB because of its partial-bind / late-fence-retire pattern. The descriptor-array slot rotation is orthogonal to the heap-region tag (which stays absolute frame index, freed by `FreeTag(N-2)`). The three cross-set co-batched UBO writes (`Global b0 + Grid b0`, `Global b5 + GTAOMain b2`, `4× PP b2`) preserve their atomic-write invariant — both/all writes use the same cached `slot`.

> **Subsystem ownership (`render-pipeline-subsystems`).** Each Vulkan descriptor Set's full lifecycle (layout + pool + per-view set + binding writes + per-frame upload) lives in one subsystem under `luth/source/luth/renderer/subsystems/`:
>
> | Set / domain | Subsystem |
> |---|---|
> | Set 0 (Global) | `GlobalSubsystem` (UpdateUBO writes binding 0 + Grid binding 0 atomically — both share the same heap region) |
> | Set 3 (Lighting + shadow) | `LightingSubsystem` (also owns IBL + shadow map + skybox) |
> | Set 5 (GPUObjectData) + cull descriptor | `GeometrySubsystem` (also owns PBR + DepthPrepass pipelines + entity↔SSBO maps) |
> | GTAO compute (3 layouts) | `GTAOSubsystem` (UpdateUBO writes Set 0 binding 5 + GTAOMain binding 2 atomically) |
> | PostProcess (4 sets, shared layout) | `PostProcessSubsystem` (bloom + tonemap pipelines, `UpdateUBO` rebinds binding 2 of all 4 sets) |
> | Outline + Grid | `EditorOverlaysSubsystem` (also owns SelectionMask pipelines + 3 `Add*Pass`) |
>
> Sets 1 (bindless), 2 (Material), 4 (BoneMatrixBuffer) live outside the subsystem split — owned by their respective scene-side systems. `RenderPipeline` is now a ~650-LOC orchestrator: holds the 6 subsystem instances, dispatches `Init`/`Shutdown`/`Update`/`Add*Pass` in dependency order, owns frame-scratch state (`m_CurrentView`, `m_CurrentViewResources`, `m_ViewResources` map), `AddImGuiPass`, named-texture registry, shader-reload dispatcher, and frame-debugger forwarders. Friend declarations between `RenderPipeline` ↔ `RenderingSystem` and `RenderPipeline` ↔ `FrameDebuggerContext` fully removed.

## Current RenderGraph Pass Order

```
(per shadow cascade × 4)
CullComputePass (shadow) ─┐
ShadowPass (depth-only)   ─┘

DepthPrepass (depth-only, main view, indirect draw)
  ↓
GTAO: PrefilterPass → MainPass (horizon integral) → DenoisePass (bilateral)
  ↓
CullComputePass (main scene) — populates per-draw indirect args
  ↓
GeometryPass (PBR forward — opaque/cutout/transparent variants, reads prepass depth + AO + shadows)
  ↓
SelectionMaskPass (entity-ID → mask for outline)
  ↓
SkyboxPass (depth = 1.0 trick, HDR)
  ↓
GridPass (optional, editor-only overlay)
  ↓
BloomExtractPass → BloomBlurH/V (separable 9-tap Gaussian, half-res)
  ↓
PostProcessPass (tonemap + bloom compose + vignette + grain + CA → LDR)
  ↓
OutlinePass (reads mask + depth, composites onto LDR)
  ↓
ImGuiPass (composites editor UI onto LDR → swapchain)
```

Pass invocations live in `RenderPipeline.cpp::BuildGraph` (chain visible at lines ~400–442). All passes go through the render graph for barrier insertion + dead-pass culling.

## Target RenderGraph Pass Order (post-`rt-renderer` arc)

End state targeted by the `rt-renderer` series (v3.0.0). RT-mandatory; the raster CSM path retires in Phase B.3.

```
DepthPrepass (depth-only, main view, indirect draw)
  ↓
CullComputePass (main scene) — populates per-draw indirect args
  ↓
SlimGBufferPass (normal RG16F + roughness R8 + motion vectors RG16F + material ID R16U; reads prepass depth)
  ↓
GTAO chain (PrefilterPass → MainPass → DenoisePass) — half-res AO; supplements RT GI's slow indirect convergence
  ↓
ClusterBuildPass (compute, async-compute eligible)
  ↓
LightAssignPass (compute, async-compute eligible)
  ↓
VolumetricInjectPass (compute) — voxel density + in-scattering from cluster lights + RT shadow rays per cell
  ↓
VolumetricIntegratePass (compute) — accumulate along view rays into froxel volume
  ↓
TlasBuildPass (per-frame for dynamic instances; static skipped)
  ↓
RtDirectLightingPass (ReSTIR DI — reservoir-sampled visibility handles both light selection and shadow)
  ↓
RtGiPass (ReSTIR GI — indirect bounce reservoirs)
  ↓
SvgfDenoisePass (spatial + temporal accumulation for RT outputs)
  ↓
GeometryPass (forward, clustered light loop, reads RT direct + GI, samples volumetric)
  ↓
SkyboxPass (depth = 1.0 trick, HDR)
  ↓
TransparentPass (forward, reads depth, reads/writes HDR; cluster lights + volumetric)
  ↓
RtReflectionsPass (stochastic ray dispatch + dedicated denoise)
  ↓
GpuParticleSimPass (compute) → ParticleRenderPass (alpha-blended sprites)
  ↓
SelectionMaskPass (entity-ID → mask for outline)
  ↓
TaaResolvePass (Karis14 — reads motion vectors from slim G-buffer, reads history)
  ↓
BloomExtractPass → BloomBlurH/V (separable 9-tap, half-res)
  ↓
PostProcessPass (tonemap [ACES variant / AgX] + bloom compose + vignette + grain + CA → LDR)
  ↓
OutlinePass (reads mask + depth, composites onto LDR)
  ↓
GridPass (editor overlay)
  ↓
ImGuiPass (composites editor UI onto LDR → swapchain)
```

> Pass-order details may shift per phase landing. Phase B.3 may ship a standalone `RtSunShadowsPass` that consolidates into ReSTIR DI when Phase C.1 lands — both shapes are correct. Phase C.1/C.3 integration may merge `RtDirectLightingPass`/`RtGiPass` into a single resampling pass.

## Active modernization — `rt-renderer` arc (v3.0.0)

The arc layers new pass families onto the existing render graph; foundational systems (job system, render graph DAG, frame pipeline, memory primitives, queue topology) stay unchanged. Key data-structure additions by phase:

**Phase A — modern foundation**
- Set 1 (bindless) scales up; new bindless sampler array + bindless mesh access via `VK_KHR_buffer_device_address`
- Set 3 reshapes: fixed `LightUBO` → unbounded `Light SSBO` + `Cluster grid SSBO` + `Light index SSBO`, all from `GPUTaggedPageAllocator`
- Slim G-buffer attachments (normal/roughness/motion vectors/material ID) — feeds TAA + RT denoising
- Volumetric voxel volume (compute storage image, frustum-aligned)

**Phase B — RT foundation**
- New descriptor set for RT (TLAS binding + RT-output storage images + SBT)
- BLAS per mesh asset (built on import or geometry change); TLAS rebuilt per frame from scene instance list
- Raster shadow pipeline (ShadowPass + cascade selection in `LightingSubsystem`) retires when B.3 lands

**Phase C — RT GI**
- ReSTIR reservoir buffers (per-pixel ping-pong, `GPUTaggedPageAllocator`)
- SVGF history buffers (color + moments, ping-pong) behind an `IDenoiser` abstraction for later NRD swap

**Phase C.5 — PT reference mode**
- Parallel `PathTraceMode` toggle; reuses Phase B BLAS/TLAS + Phase C material BRDF sampling; persistent accumulation image; bypasses raster + ReSTIR

**Phase D — reflections + atmospheric polish**
- RT reflections (stochastic samples + denoise outputs)
- Volumetric voxel volume gains RT shadow-ray writes per cell
- GPU particle simulation/render passes added downstream of GeometryPass

Full per-phase work in `docs/development/epics/rt-renderer.md` (local spec, never committed).

## Memory Budget

| Buffer | Size |
|--------|------|
| Material SSBO | 16384 × 64B = 1 MB |
| Light UBO | 64 × 32B + 16B ≈ 2.1 KB |
| Global UBO | 3×mat4 + vec3 + float ≈ 200B |
| Shadow Map | 2048² × 4B = 16 MB |
| GBuffer (4 RTs) | 1920×1080 × (8+8+4+4)B ≈ 50 MB |
| SSAO noise tex | 4×4 × 12B = negligible |
| BRDF LUT | 512² × 8B = 2 MB |
