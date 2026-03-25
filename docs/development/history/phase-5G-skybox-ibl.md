# Phase 5-G — Skybox + IBL

**Date:** 2026-03-23
**Status:** Complete

## Summary

Implemented full image-based lighting (IBL) for PBR and an HDR environment skybox. The environment map is precomputed via four compute shader dispatches at startup, results are stored in dedicated Set 0 bindings, and the PBR shader switches from a constant ambient term to a physically-based split-sum IBL calculation.

---

## What Was Built

### Cubemap Support (VKTexture)
- New constructor: `VKTexture(width, height, format, arrayLayers, createFlags, mipLevels, extraUsage)`
- `CreateViewAndSampler` detects cubemaps (`arrayLayers == 6 && CUBE_COMPATIBLE`) and creates `VK_IMAGE_VIEW_TYPE_CUBE` views
- Cubemaps skip the bindless registry; placed in dedicated Set 0 bindings instead
- `CreateMipView(mipLevel, forStorage)`: per-mip views; `forStorage=true` returns `VK_IMAGE_VIEW_TYPE_2D_ARRAY` for compute shader `image2DArray` bindings (SPIR-V Dim=2D, Arrayed=1 requires this)
- All barrier/blit subresource ranges updated from hardcoded `layerCount=1` to `m_ArrayLayers`
- Added `RG16F` to TextureFormat enum (needed for BRDF LUT)

### Set 0 Layout Expansion
`InitGlobalUniforms()` extended from 1 to 4 bindings:
- Binding 0 — `GlobalUniforms` UBO (vertex + fragment)
- Binding 1 — `irradianceMap` samplerCube (fragment)
- Binding 2 — `prefilteredMap` samplerCube (fragment)
- Binding 3 — `brdfLUT` sampler2D (fragment)

### IBL Precomputation (`InitIBLResources`)
All GPU work done via `VulkanContext::ImmediateSubmit` one-shot command buffers.

1. **HDR Load** — `stbi_loadf` with Y-flip, uploaded via staging buffer to a 1024×1024 equirectangular `sampler2D` texture
2. **Equirect → Cubemap** — `equirect_to_cubemap.comp` (16×16 workgroup, dispatch Z=6 for 6 faces), 1024×1024 env cubemap
3. **Mipmap Generation** — `vkCmdBlitImage` chain on the env cubemap for filtered sampling in prefilter dispatch
4. **Irradiance Convolution** — `irradiance_convolve.comp` (8×8 workgroup), hemisphere integration with cos-weighted sampling (Δφ = 0.025), 32×32 cubemap
5. **Pre-filtered Env Map** — `prefilter_env.comp` (16×16 workgroup), 1024 GGX importance samples per texel, 128×128 cubemap with 5 mip levels; dispatched once per mip with `roughness` push constant
6. **BRDF LUT** — `brdf_lut.comp` (16×16 workgroup), Hammersley sequence + GGX importance sampling, 512×512 RG16F output; 1024 samples per texel
7. **IBL Sampler** — Linear, clamp-to-edge, mipmap linear, maxLod=4
8. **Descriptor Writes** — Writes IBL textures to Set 0 bindings 1–3 of `m_GlobalDescriptorSet`
9. **Fallback** — Creates 1×1 placeholder textures if no HDR file is present so bindings remain valid

### Compute Shaders (new files)
| File | Purpose |
|------|---------|
| `equirect_to_cubemap.comp` | HDR equirectangular → 6-face cubemap; `GetCubemapDirection` + `DirectionToEquirect` helpers |
| `irradiance_convolve.comp` | Diffuse irradiance convolution; reads `samplerCube envMap`, writes `image2DArray` |
| `prefilter_env.comp` | Specular pre-filter; GGX importance sampling; push constant `roughness`; reads/writes as above |
| `brdf_lut.comp` | BRDF integration LUT (Schlick-GGX split-sum); outputs to `image2D` RG16F |

### Skybox Rendering Pass
- **`skybox.vert`**: removes translation (`mat4(mat3(ubo.view))`), forces depth=1.0 via `gl_Position = pos.xyww`
- **`skybox.frag`**: samples `prefilteredMap` (Set 0, binding 2) at mip 0 — mip 0 of the pre-filtered map equals the unfiltered environment
- **Pipeline**: `depthTest=true`, `depthWrite=false`, `depthCompareOp=LESS_OR_EQUAL`, `cullMode=BACK_BIT`
- **Cube mesh**: 36 position-only vertices stored in a `VKVertexBuffer`, created at init time
- **Pass order**: ShadowPass → GeometryPass → **SkyboxPass** → Bloom → PostProcess → ImGui

### SceneDepth Made Persistent
Previously transient (recreated each frame via `builder.CreateTexture`), now allocated once in the constructor and imported via `rg.ImportResource` with `storeOp=STORE`. This allows SkyboxPass to load geometry depth without a copy.

### PBR Shader IBL Integration (`pbr.frag`)
- Declared `irradianceMap` (binding 1), `prefilteredMap` (binding 2), `brdfLUT` (binding 3) in Set 0
- Added `FresnelSchlickRoughness(cosTheta, F0, roughness)` for IBL Fresnel
- Replaced constant ambient `vec3(0.03) * albedo * ao` with full split-sum IBL:
  - **Diffuse**: `irradiance * albedo * (1 - kS) * (1 - metallic)`
  - **Specular**: `prefilteredColor * (F * brdf.r + brdf.g)` where `prefilteredColor = textureLod(prefilteredMap, R, roughness * MAX_LOD)` and `brdf = texture(brdfLUT, vec2(NdotV, roughness))`

### Descriptor Pool Fix
`DescriptorAllocator::CreatePool` now includes `VK_DESCRIPTOR_TYPE_STORAGE_IMAGE` in its pool sizes, required for the compute dispatch descriptor sets.

### Frame Debugger
Added `SkyboxPass` case to the snapshot enrichment block so the frame debugger correctly reports shader name ("skybox"), draw call count (1), and pipeline state.

---

## Root Causes / Bugs Fixed During Implementation

### Skybox Invisible (Y-flip Winding Bug)
The Vulkan Y-flip (`projection[1][1] *= -1.0f`) reverses triangle winding order for all projected geometry. The skybox pipeline was originally set to `VK_CULL_MODE_FRONT_BIT` (conventional for inside-out cube rendering), but the Y-flip made the inside-visible faces appear as front faces — they were culled. Fixed by switching to `VK_CULL_MODE_BACK_BIT`.

### Compute Storage Image View Type Mismatch
Compute shaders declare `writeonly image2DArray` (SPIR-V: Dim=2D, Arrayed=1), which requires `VK_IMAGE_VIEW_TYPE_2D_ARRAY`. The original `CreateMipView` always returned `VK_IMAGE_VIEW_TYPE_CUBE` for cubemap textures. Added `forStorage` parameter to `CreateMipView`; all compute dispatch storage image outputs pass `forStorage=true`.

---

## Files Modified

| File | Change |
|------|--------|
| `luth/renderer/Texture.h` | Added `RG16F` to TextureFormat enum |
| `luth/renderer/backend/vulkan/VulkanTexture.h` | Cubemap constructor, `GetArrayLayers`, `CreateMipView(mip, forStorage)` |
| `luth/renderer/backend/vulkan/VulkanTexture.cpp` | Cubemap constructor, view type detection, `forStorage` branch, `m_ArrayLayers` in barriers/blits |
| `luth/renderer/backend/vulkan/VulkanDescriptors.cpp` | Added `STORAGE_IMAGE` pool size to `DescriptorAllocator::CreatePool` |
| `luth/scene/systems/RenderingSystem.h` | `GeometryOutput` struct, `m_SceneDepth`, IBL members, skybox members, new method declarations |
| `luth/scene/systems/RenderingSystem.cpp` | `InitGlobalUniforms` (4 bindings), `InitIBLResources` (~400 lines), `AddGeometryPass` (persistent depth), `AddSkyboxPass`, `CreatePipelines` (skybox pipeline), snapshot enrichment, `Resize` |
| `sandbox/assets/shaders/pbr.frag` | IBL sampler declarations, `FresnelSchlickRoughness`, full split-sum ambient |
| `sandbox/assets/shaders/skybox.vert` | New |
| `sandbox/assets/shaders/skybox.frag` | New |
| `sandbox/assets/shaders/equirect_to_cubemap.comp` | New |
| `sandbox/assets/shaders/irradiance_convolve.comp` | New |
| `sandbox/assets/shaders/prefilter_env.comp` | New |
| `sandbox/assets/shaders/brdf_lut.comp` | New |
