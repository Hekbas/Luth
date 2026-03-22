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

        bool operator==(const PipelineKey& other) const
        {
            return shaderUUID == other.shaderUUID && renderMode == other.renderMode;
        }
    };

    struct PipelineKeyHash
    {
        size_t operator()(const PipelineKey& key) const noexcept
        {
            size_t h1 = UUIDHash{}(key.shaderUUID);
            size_t h2 = static_cast<size_t>(key.renderMode);
            return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
        }
    };

    class PipelineManager
    {
    public:
        using ConfigFactory = std::function<PipelineConfig(Material::RenderMode)>;

        void Init(const std::vector<VkDescriptorSetLayout>& layouts, ConfigFactory factory);
        void Shutdown();

        VKPipeline* GetOrCreate(const UUID& shaderUUID, Material::RenderMode mode,
                                const std::vector<u32>& vertSpv, const std::vector<u32>& fragSpv);

        void InvalidateShader(const UUID& shaderUUID);
        void Clear();

    private:
        std::unordered_map<PipelineKey, std::unique_ptr<VKPipeline>, PipelineKeyHash> m_Pipelines;
        std::vector<VkDescriptorSetLayout> m_Layouts;
        ConfigFactory m_ConfigFactory;
    };
}
