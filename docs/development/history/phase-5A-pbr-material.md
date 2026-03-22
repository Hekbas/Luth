# Phase 5-A: PBR Shader + Material Pipeline ✅ (2026-03-15)

Cook-Torrance BRDF with GGX normal distribution, Smith geometry, and Fresnel-Schlick approximation. Material data uploaded via SSBO (Set 2) indexed by push constant `materialIndex`.

### Files Created/Modified

| File | Changes |
|---|---|
| `sandbox/assets/shaders/pbr.vert` | Full vertex attributes (position, normal, UV0, UV1, tangent), TBN matrix output |
| `sandbox/assets/shaders/pbr.frag` | Cook-Torrance PBR: GGX D, Smith G, Fresnel-Schlick F, bindless texture sampling |
| `luth/renderer/Material.cpp` | `UpdateGPUData()` — walk maps, populate bindless indices + flags bitmask |
| `luth/scene/systems/RenderingSystem.cpp` | Material SSBO (16384 × GPUMaterialData, Set 2), per-RenderMode pipeline variants, sorted draw calls |
