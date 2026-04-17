# Phase 5-D: Post-Processing Stack ✅ (2026-03-22)

**Goal:** HDR pipeline with bloom, tonemapping, and visual effects (vignette, grain, chromatic aberration, color grading).

### Architecture

Forward HDR pipeline using fullscreen triangle technique (vertex positions generated from `gl_VertexIndex`, no vertex buffer needed). SceneColor upgraded from RGBA8 to RGBA16F. A new LDR output texture (RGBA8) receives the final tonemapped result for ScenePanel display.

**Pass order:** `ShadowPass → GeometryPass → BloomExtract → BloomBlurH → BloomBlurV → PostProcess → ImGuiPass`

### Bloom

Two-stage: bright pixel extraction (luminance threshold with soft knee) → separable 9-tap (5-weight) Gaussian blur at half resolution. Two persistent bloom textures (BloomA, BloomB) imported as external RenderGraph resources each frame. Ping-pong: Extract→BloomA, BlurH→BloomB, BlurV→BloomA.

### Tonemapping + Effects

PostProcess composite shader applies in order: chromatic aberration → bloom composite → exposure → tonemapping → contrast → saturation → vignette → grain → gamma correction. Four tonemapping operators: Linear, Reinhard, ACES Filmic, Uncharted 2. All parameters driven by `PostProcessSettings` struct uploaded as std140 UBO.

### Resources

3 fullscreen pipelines (no vertex input, no depth, cull none), dedicated descriptor pool (4 sets), LINEAR/CLAMP_TO_EDGE sampler. Bloom textures recreated at half viewport resolution on resize.

### Files

| File | Changes |
|---|---|
| `luth/renderer/PostProcessSettings.h` | NEW — Settings + GPU UBO structs |
| `sandbox/assets/shaders/fullscreen.vert` | NEW — Fullscreen triangle vertex shader |
| `sandbox/assets/shaders/bloomExtract.frag` | NEW — Bright pixel extraction |
| `sandbox/assets/shaders/bloomBlur.frag` | NEW — Separable Gaussian blur |
| `sandbox/assets/shaders/postprocess.frag` | NEW — Composite + tonemap + effects |
| `luth/renderer/Texture.h` | RGBA16F enum |
| `luth/renderer/backend/vulkan/VulkanTexture.cpp` | RGBA16F format mapping |
| `luth/renderer/rendergraph/RenderGraphResources.h` | RGBA16_Float enum |
| `luth/renderer/rendergraph/RenderGraph.cpp` | RGBA16_Float in GetVkFormat() |
| `luth/renderer/rendergraph/RenderResourceCache.cpp` | RGBA16_Float in GetTexture() |
| `luth/scene/systems/RenderingSystem.h/.cpp` | HDR upgrade, bloom/PP resources, 4 new passes, resize, UBO upload |
| `luth/editor/panels/RenderPanel.cpp` | Post-processing UI wired to all settings |
