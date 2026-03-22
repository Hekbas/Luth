# Phase 5-G — Skybox + Image-Based Lighting (IBL)

**Goal:** IBL-compatible skybox for physically correct ambient lighting.

## 5-G.1 Skybox Rendering

- Cube mesh (unit cube) drawn with reversed depth test (`VK_COMPARE_OP_LESS_OR_EQUAL`, depth write off)
- `skybox.vert`: transform with `mat4(mat3(view)) * proj` — removes translation
- `skybox.frag`: samples `samplerCube` environment texture

## 5-G.2 Cubemap Import

Add `CubemapImporter` that accepts:
- 6 individual face images (px/nx/py/ny/pz/nz)
- Equirectangular HDR image (most common with `.hdr` files)

For equirectangular → cubemap: offline conversion pass at import time using a compute shader.

## 5-G.3 Image-Based Lighting (IBL)

At cubemap load time, precompute:
1. **Irradiance map** (2×2 convolution, 32×32 cubemap) — diffuse IBL
2. **Pre-filtered environment map** (mip levels = roughness levels, 128×128 base) — specular IBL
3. **BRDF LUT** (2D, 512×512, precomputed once) — stored as engine asset

In `pbr.frag`:
```glsl
layout(set = 0, binding = 2) uniform samplerCube irradianceMap;
layout(set = 0, binding = 3) uniform samplerCube prefilteredMap;
layout(set = 0, binding = 4) uniform sampler2D   brdfLUT;

// Diffuse IBL
vec3 irradiance = texture(irradianceMap, N).rgb;
vec3 diffuseIBL = irradiance * albedo;

// Specular IBL
float lod = material.roughness * MAX_REFLECTION_LOD;
vec3 prefilteredColor = textureLod(prefilteredMap, R, lod).rgb;
vec2 brdf = texture(brdfLUT, vec2(NdotV, material.roughness)).rg;
vec3 specularIBL = prefilteredColor * (F * brdf.x + brdf.y);

vec3 ambient = (kD * diffuseIBL + specularIBL) * ao;
```

## Files to create/modify

- `sandbox/assets/shaders/skybox.vert` *(new)*
- `sandbox/assets/shaders/skybox.frag` *(new)*
- `sandbox/assets/shaders/irradiance_convolve.comp` *(new)*
- `sandbox/assets/shaders/prefilter_env.comp` *(new)*
- `luth/renderer/Cubemap.h/.cpp` *(new)*
- `luth/resources/importers/CubemapImporter.h/.cpp` *(new)*
- `luth/scene/systems/RenderingSystem.h/.cpp` — SkyboxPass, IBL resources
- `sandbox/assets/shaders/pbr.frag` — IBL ambient term
