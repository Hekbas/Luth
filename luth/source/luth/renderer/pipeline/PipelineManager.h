#pragma once

#include "luth/core/UUID.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include <functional>

namespace Luth
{
    // Caches VKPipeline instances keyed by (shader UUID, render mode, cull mode, polygon mode).
    // First lookup triggers a lazy compile. On shader reload, DeferredInvalidateShader marks
    // affected entries so the next frame rebuilds fresh pipelines without stalling the live one.
    struct PipelineKey
    {
        UUID shaderUUID;
        Material::RenderMode renderMode;
        Material::CullMode cullMode;
        VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;

        bool operator==(const PipelineKey& other) const
        {
            return shaderUUID == other.shaderUUID && renderMode == other.renderMode
                && cullMode == other.cullMode && polygonMode == other.polygonMode;
        }
    };

    struct PipelineKeyHash
    {
        size_t operator()(const PipelineKey& key) const noexcept
        {
            size_t h1 = UUIDHash{}(key.shaderUUID);
            size_t h2 = static_cast<size_t>(key.renderMode);
            size_t h3 = static_cast<size_t>(key.cullMode);
            size_t h4 = static_cast<size_t>(key.polygonMode);
            size_t combined = h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
            combined = combined ^ (h3 * 0x517cc1b727220a95ULL + 0x9e3779b9 + (combined << 6) + (combined >> 2));
            return combined ^ (h4 * 0x6c62272e07bb0142ULL + 0x9e3779b9 + (combined << 6) + (combined >> 2));
        }
    };

    class PipelineManager
    {
    public:
        using ConfigFactory = std::function<PipelineConfig(Material::RenderMode, Material::CullMode, VkPolygonMode)>;

        void Init(const std::vector<VkDescriptorSetLayout>& layouts, ConfigFactory factory);
        void Shutdown();

        VKPipeline* GetOrCreate(const UUID& shaderUUID, Material::RenderMode mode, Material::CullMode cullMode,
                                VkPolygonMode polygonMode,
                                const std::vector<u32>& vertSpv, const std::vector<u32>& fragSpv);

        void InvalidateShader(const UUID& shaderUUID);
        void Clear();

        // Deferred variants — release each cached pipeline into VulkanContext's
        // per-frame deletion queue instead of destroying synchronously. Used by
        // the shader-reload path so old pipelines outlive their last in-flight
        // command buffer (drained MAX_FRAMES_IN_FLIGHT frames later in AcquireImage).
        void DeferredInvalidateShader(const UUID& shaderUUID);
        void DeferredClear();

    private:
        std::unordered_map<PipelineKey, std::unique_ptr<VKPipeline>, PipelineKeyHash> m_Pipelines;
        std::vector<VkDescriptorSetLayout> m_Layouts;
        ConfigFactory m_ConfigFactory;
    };
}
