# Phase 13 — Cascaded Shadow Maps

**Version:** v1.3.0  |  **Date:** 2026-04-16  |  **Epic:** #60

---

## What Was Built

Replaced the single 2048² directional-light shadow map with a 4-cascade PSSM system:

- **4-layer shadow array** — `VK_IMAGE_VIEW_TYPE_2D_ARRAY` D32 texture (2048×2048×4); per-layer `VkImageView` for ShadowPass writes; full-array view for PBR sampling.
- **CSM uniform plumbing** — `GlobalUniforms` extended with `lightSpaceMatrix[4]`, `cascadeSplitsViewZ`, `shadowBias`, `shadowNormalBias`, `cascadeTexelSize`, `cascadeBlendWidth`, `debugVisualizeCascades` (std140, 544 B).
- **PSSM splits** — Engel practical formula (`splitLambda = 0.5` default) converted to view-space Z.
- **Per-cascade ortho fitting** — Sascha Willems bounding-sphere approach: centroid of 8 sub-frustum corners, `radius = ceil(r * 16) / 16`, `glm::lookAt` + symmetric `glm::ortho(-r, r, -r, r, 0, 2r)`. Rotation-invariant and shimmer-resistant.
- **ShadowPass multi-layer** — 4 × `AddShadowPass` calls, each with per-layer view and `cascadeIndex` push constant.
- **Per-cascade GPU culling** — Indirect buffer region per cascade; 5 cull dispatches (camera + 4 cascades); frustum planes extracted via Gribb-Hartmann from each `lightSpaceMatrix[i]`.
- **PBR cascade selection** — `viewZ` → primary cascade; inside-test fall-through loop for robustness; 3×3 PCF via `sampler2DArrayShadow`; cascade blend at transition zone; per-cascade depth + normal bias scaled by `cascadeTexelSize`.
- **Debug viz** — `DebugVisualizeCascades` flag tints fragments by cascade index (red/green/blue/yellow).

## Known Issue — Coverage Gaps

A light-direction-dependent coverage bug persists: large ground-plane regions fail the `ProjectInCascade` inside-test (specifically `proj.z < 0`), appearing unlit. The bug is **not** in the cascade-fit math — Sascha's verbatim reference implementation also produces the symptom. Cascade-tint visualization confirms the fit geometry is correct; the failure is in shadow-map sampling.

**Prioritized suspects for next session:**
1. UBO round-trip integrity — verify shader receives the correct `lightSpaceMatrix` values (sentinel matrix test).
2. Shadow pass `cullMode = VK_CULL_MODE_FRONT_BIT` — Sascha's reference uses back-face cull; mismatch may corrupt depth writes.
3. Gribb-Hartmann near plane uses GL clip convention (`row3 + row2`) rather than Vulkan (`row3`); too permissive for culling but worth fixing.

**Recommended tooling:** Frame Debugger needs to be updated before tackling this (it was unavailable during Phase 13 debugging and would have resolved the issue quickly).

## Files Modified

- `luth/source/luth/scene/systems/RenderingSystem.{h,cpp}` — GlobalUniforms, shadow resources, cascade math, per-cascade cull dispatches
- `luth/source/luth/renderer/passes/ShadowPass.cpp` — per-layer view, cascadeIndex push constant
- `luth/source/luth/renderer/passes/CullPass.{h,cpp}` — destOffset arg, 5 named dispatches
- `luth/source/luth/renderer/passes/GeometryPass.cpp` — all-layer barrier before geometry
- `luth/source/luth/renderer/backend/vulkan/VulkanTexture.{h,cpp}` — `CreateLayerView`
- `luth/source/luth/scene/Components.h` — `DirectionalLight` CSM fields
- `luth/source/luth/scene/SceneSerializer.cpp` — persist CSM fields
- `luth/source/luth/editor/panels/InspectorPanel.cpp` — basic CSM inspector controls
- `luth/assets/shaders/pbr.frag` — cascade selection, blending, PCF, debug tint
- `luth/assets/shaders/shadowDepth.vert` — `cascadeIndex` push constant
- `luth/assets/shaders/shadowDepth_skinned.vert` — same
- `luth/assets/shaders/gpu_cull.comp` — `destOffset` push constant
- `luth/source/luth/core/Version.h` — bumped to v1.3.0
