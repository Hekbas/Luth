# Rendering Pipeline — Architecture Details

## Descriptor Set Allocation

| Set | Content | Updated |
|-----|---------|---------|
| 0 | GlobalUniforms + shadow cascade array + IBL irradiance + IBL prefiltered env + BRDF LUT + GTAO settings UBO (6 bindings) | Per frame — cycled across `MAX_FRAMES_IN_FLIGHT` slots |
| 1 | Bindless textures (16384 slots) | On upload-fence retire (UPDATE_AFTER_BIND, partially-bound; deferred via `UploadContext` pump per `texture-async-uploads` v2.8.14) |
| 2 | Material SSBO (16384 entries) | Per game stage — rebound to fresh `GPUTaggedPageAllocator` region (UPDATE_AFTER_BIND) |
| 3 | Light UBO (dir + point lights) + shadow map sampler | Per frame — cycled across `MAX_FRAMES_IN_FLIGHT` slots |
| 4 | `BoneMatrixBuffer` SSBO (per-entity skinning blocks) | Per game stage — rebound to fresh tagged-heap region (UPDATE_AFTER_BIND) |
| 5 | `GPUObjectData` SSBO — per-draw transforms/IDs for indirect dispatch | Per render stage — cycled across `MAX_FRAMES_IN_FLIGHT` slots |

> Set 0 expanded from 4 → 6 bindings across `csm` (v1.3.0 — cascade array) and `gtao` (v1.5.0 — AO sampler + settings UBO). Set 4 added by `animation-gpu-skinning`; Set 5 by `compute-gpu-culling` (v1.2.0). Sets 2/4/5 moved to per-stage rebind in `gpu-tagged-heap` (v2.8.10) — backing storage allocated each frame from `GPUTaggedPageAllocator`, descriptors rewritten via `vkUpdateDescriptorSets`. The cull descriptor (binding into Set 5 + Indirect Buffer for compute) follows the same pattern. Set 1 bindless registration moved from synchronous-in-VKTexture-ctor to a `UploadContext` pending-bind pump in `texture-async-uploads` (v2.8.14) — `VKTexture` ctor pushes `{outIndex, view, sampler, fence}`; pump drains in `AssetManager::Update` once `IsComplete(fence)` and writes the slot through `outIndex`. Until then `m_BindlessIndex == INVALID_BINDLESS_SLOT` and `Material::BindlessOrNull` keeps shaders on reserved white slot 0. `~VKTexture` cancels by view-handle.

> **Per-frame descriptor cycling (`per-frame-descriptor-set-cycling` v2.9.13).** Sets whose bindings are rewritten per render-stage to fresh tagged-heap regions (Set 0, Set 3, Set 5, Cull, 4 PostProcess sets, GTAO main, Grid) are now `std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>` — each frame writes its own slot, indexed by `Renderer::GetFrameData()->GetRenderFrameIndex() % MAX_FRAMES_IN_FLIGHT` at bind/update sites. The descriptor-array slot rotation is orthogonal to the heap-region tag (which stays absolute frame index, freed by `FreeTag(N-2)`). Cycling makes the v2.9.11 `UPDATE_AFTER_BIND` workaround unnecessary for these bindings — the slot a frame writes is never the slot the GPU is consuming, gated by the existing `m_FrameTimeline.Wait(N - MAX_FRAMES_IN_FLIGHT + 1)` in `VulkanBackend::AcquireImage`. UAB remains in use for Set 1 (bindless, partially-bound) and Sets 2/4 (game-stage tagged, different cycling cadence). The three cross-set co-batched UBO writes (`Global b0 + Grid b0`, `Global b5 + GTAOMain b2`, `4× PP b2`) preserve their atomic-write invariant — both/all writes use the same cached `slot` so the next frame's allocator doesn't overwrite a region the previous frame's binding still references.

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

## Target RenderGraph Pass Order (End State)

```
ShadowPass (depth-only, light POV)
  ↓
GBuffer Pass (albedo RT0, normal RT1, metalRough RT2, depth)
  ↓
SSAOPass (read depth+normals → occlusion R8)
  ↓
SSAOBlurPass
  ↓
LightingPass (deferred: GBuffer + occlusion + shadow → HDR RGBA16F)
  ↓
SkyboxPass (read/write HDR)
  ↓
TransparentPass (forward, read depth, read/write HDR)
  ↓
BloomExtractPass → DownsamplePass × N → UpsamplePass × N
  ↓
PostProcessPass (tonemap + bloom + vignette + grain + CA + FXAA)
  ↓
ImGuiPass (swapchain)
```

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
