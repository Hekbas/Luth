#pragma once

#include "luth/core/UUID.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include <functional>

namespace Luth
{
    struct PipelineKey
    {
        UUID shaderUUID;
        Material::RenderMode renderMode;
        Material::CullMode cullMode;

        bool operator==(const PipelineKey& other) const
        {
            return shaderUUID == other.shaderUUID && renderMode == other.renderMode && cullMode == other.cullMode;
        }
    };

    struct PipelineKeyHash
    {
        size_t operator()(const PipelineKey& key) const noexcept
        {
            size_t h1 = UUIDHash{}(key.shaderUUID);
            size_t h2 = static_cast<size_t>(key.renderMode);
            size_t h3 = static_cast<size_t>(key.cullMode);
            size_t combined = h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
            return combined ^ (h3 * 0x517cc1b727220a95ULL + 0x9e3779b9 + (combined << 6) + (combined >> 2));
        }
    };

    class PipelineManager
    {
    public:
        using ConfigFactory = std::function<PipelineConfig(Material::RenderMode, Material::CullMode)>;

        void Init(const std::vector<VkDescriptorSetLayout>& layouts, ConfigFactory factory);
        void Shutdown();

        VKPipeline* GetOrCreate(const UUID& shaderUUID, Material::RenderMode mode, Material::CullMode cullMode,
                                const std::vector<u32>& vertSpv, const std::vector<u32>& fragSpv);

        void InvalidateShader(const UUID& shaderUUID);
        void Clear();

    private:
        std::unordered_map<PipelineKey, std::unique_ptr<VKPipeline>, PipelineKeyHash> m_Pipelines;
        std::vector<VkDescriptorSetLayout> m_Layouts;
        ConfigFactory m_ConfigFactory;
    };
}
