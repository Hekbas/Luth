# Phase 5-B: Lighting System + Shadows ✅ (2026-03-15)

ECS-driven lighting with UBO (Set 3, binding 0): 1 directional + 64 point lights. Shadow mapping via dedicated depth-only pass (2048² D32) with PCF 3×3 filtering.

### Files Created/Modified

| File | Changes |
|---|---|
| `sandbox/assets/shaders/shadowDepth.vert` | Depth-only vertex shader for shadow pass |
| `sandbox/assets/shaders/shadowDepth.frag` | Empty fragment shader (depth-only) |
| `sandbox/assets/shaders/pbr.frag` | Added `LightUBO` (Set 3), `ComputeShadow()` with PCF 3×3, `sampler2DShadow` (Set 3 binding 1) |
| `luth/scene/systems/RenderingSystem.h` | `GlobalUniforms` — added `lightSpaceMatrix` |
| `luth/scene/systems/RenderingSystem.cpp` | `UpdateLightUniforms()`, `ShadowPass` in RenderGraph, orthographic light-space matrix |
| `luth/renderer/backend/vulkan/VulkanTexture.cpp` | Depth formats: correct `VkFormat` + `SAMPLED_BIT` |
| `luth/renderer/backend/vulkan/VulkanPipeline.cpp` | `colorBlending.attachmentCount` derived from `colorFormats.size()` (depth-only = 0) |
