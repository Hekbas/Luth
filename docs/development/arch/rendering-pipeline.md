# Rendering Pipeline — Architecture Details

## Descriptor Set Allocation

| Set | Content | Updated |
|-----|---------|---------|
| 0 | GlobalUniforms (view/proj/camera) + shadowMap + IBL | Per frame |
| 1 | Bindless textures (16384 slots) | On texture load |
| 2 | Material SSBO (16384 entries) | Per frame if dirty |
| 3 | Light UBO (dir + point lights) | Per frame |

## Current RenderGraph Pass Order

```
ShadowPass (depth-only, light POV)
  ↓
GeometryPass (PBR forward, 3 pipeline variants)
  ↓
BloomExtractPass (bright pixels → half-res)
  ↓
BloomBlurH → BloomBlurV (separable 9-tap Gaussian)
  ↓
PostProcessPass (tonemap + bloom compose + effects → LDR)
  ↓
ImGuiPass (swapchain)
```

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
