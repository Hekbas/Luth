# Phase 5-F: Pipeline Cache + Variants ✅ (2026-03-22)

**Goal:** Avoid redundant pipeline compilation on startup; centralize pipeline variant management keyed by shader + render state.

### Architecture

Two-layer system: **PipelineCache** (Vulkan-level) handles binary blob persistence, while **PipelineManager** (engine-level) manages typed pipeline variants with lazy creation and targeted invalidation.

On startup, `PipelineCache::Init()` reads `cache/pipeline.bin` and passes the blob to `vkCreatePipelineCache`. On shutdown, `vkGetPipelineCacheData` extracts the compiled data and writes it back to disk. The Vulkan spec guarantees stale or incompatible cache data is silently ignored, so no validation is needed.

`PipelineManager` replaces the ad-hoc `unordered_map<RenderMode, unique_ptr<VKPipeline>>` in RenderingSystem with a proper factory keyed by `{shaderUUID, renderMode, cullMode, polygonMode}`. A `ConfigFactory` lambda encodes per-mode pipeline differences (depth write, blending, cull mode). `GetOrCreate()` lazily compiles on first request; `InvalidateShader()` removes all variants for a given shader UUID on hot-reload.

### Key Design Decisions

- **Static singleton for PipelineCache**: Matches VulkanContext pattern; cache handle needed globally by all VKPipeline constructors
- **PipelineKey includes cullMode + polygonMode**: Extends beyond original spec to support per-material overrides, not just per-RenderMode
- **FNV-1a-like hash mixing**: Combines UUID hash with enum casts using `0x9e3779b97f4a7c15` golden ratio constant for good distribution
- **ConfigFactory pattern**: Decouples pipeline configuration logic from PipelineManager, keeping it in RenderingSystem where rendering knowledge lives
- **Targeted invalidation**: `InvalidateShader(uuid)` only destroys variants for the reloaded shader, leaving shadow/post-process pipelines untouched

### Files

| File | Changes |
|---|---|
| `luth/renderer/backend/vulkan/PipelineCache.h/.cpp` | NEW — VkPipelineCache disk persistence (`cache/pipeline.bin`) |
| `luth/renderer/PipelineManager.h/.cpp` | NEW — Pipeline variant cache keyed by `{shaderUUID, renderMode, cullMode, polygonMode}` |
| `luth/renderer/backend/vulkan/VulkanPipeline.cpp` | Pass `PipelineCache::Get()` to `vkCreateGraphicsPipelines()` |
| `luth/renderer/backend/vulkan/VulkanBackend.cpp` | `PipelineCache::Init()` / `Shutdown()` lifecycle |
| `luth/scene/systems/RenderingSystem.h/.cpp` | Replaced ad-hoc pipeline map with `PipelineManager` |
