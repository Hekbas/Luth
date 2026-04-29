# Rendering Pipeline — Architecture Details

## Descriptor Set Allocation

| Set | Content | Updated |
|-----|---------|---------|
| 0 | GlobalUniforms + shadow cascade array + IBL irradiance + IBL prefiltered env + BRDF LUT + GTAO settings UBO (6 bindings) | Per frame |
| 1 | Bindless textures (16384 slots) | On texture load (UPDATE_AFTER_BIND, partially-bound) |
| 2 | Material SSBO (16384 entries) | Per game stage — rebound to fresh `GPUTaggedPageAllocator` region (UPDATE_AFTER_BIND) |
| 3 | Light UBO (dir + point lights) + shadow map sampler | Per frame |
| 4 | `BoneMatrixBuffer` SSBO (per-entity skinning blocks) | Per game stage — rebound to fresh tagged-heap region (UPDATE_AFTER_BIND) |
| 5 | `GPUObjectData` SSBO — per-draw transforms/IDs for indirect dispatch | Per render stage — rebound to fresh tagged-heap region (UPDATE_AFTER_BIND) |

> Set 0 expanded from 4 → 6 bindings across `csm` (v1.3.0 — cascade array) and `gtao` (v1.5.0 — AO sampler + settings UBO). Set 4 added by `animation-gpu-skinning`; Set 5 by `compute-gpu-culling` (v1.2.0). Sets 2/4/5 moved to per-stage rebind in `gpu-tagged-heap` (v2.8.10) — backing storage allocated each frame from `GPUTaggedPageAllocator`, descriptors rewritten via `vkUpdateDescriptorSets` (UPDATE_AFTER_BIND_BIT). The cull descriptor (binding into Set 5 + Indirect Buffer for compute) follows the same pattern.

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
