# Phase 5-F — Pipeline Cache + Variants

**Goal:** Avoid redundant pipeline compilation, support runtime pipeline variants.

## 5-F.1 VkPipelineCache

Currently `VK_NULL_HANDLE` is passed to `vkCreateGraphicsPipelines`.

```cpp
class PipelineCache {
public:
    static void Init();
    static void Save(const fs::path& cacheFile);
    static void Load(const fs::path& cacheFile);
    static VkPipelineCache Get();
private:
    static VkPipelineCache s_Cache;
};
```

On engine shutdown: `vkGetPipelineCacheData` + write to `cache/pipeline.bin`.
On startup: read from file + `vkCreatePipelineCache` with `initialDataSize`.

## 5-F.2 Pipeline Variants

Two axes of variation:
1. **Shader** (pbr, unlit, custom)
2. **RenderMode** (Opaque, Cutout, Transparent)

Key:
```cpp
struct PipelineKey {
    UUID shaderUUID;
    Material::RenderMode renderMode;
    // Potentially: VkFormat colorFormat, depthFormat
};
```

`PipelineManager` maintains `unordered_map<PipelineKey, VKPipeline*>`.
On `GetOrCreate(key)`: compile if absent, return cached.

## Files to modify

- `luth/renderer/PipelineCache.h/.cpp` *(new)*
- `luth/renderer/PipelineManager.h/.cpp` *(new)*
- `luth/renderer/backend/vulkan/VulkanPipeline.cpp` — pass cache handle
- `luth/scene/systems/RenderingSystem.cpp` — use PipelineManager
