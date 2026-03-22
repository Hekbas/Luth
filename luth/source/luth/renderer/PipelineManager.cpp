#include "luthpch.h"
#include "PipelineManager.h"

namespace Luth
{
    void PipelineManager::Init(const std::vector<VkDescriptorSetLayout>& layouts, ConfigFactory factory)
    {
        m_Layouts = layouts;
        m_ConfigFactory = std::move(factory);
    }

    void PipelineManager::Shutdown()
    {
        Clear();
    }

    VKPipeline* PipelineManager::GetOrCreate(const UUID& shaderUUID, Material::RenderMode mode,
                                              const std::vector<u32>& vertSpv, const std::vector<u32>& fragSpv)
    {
        PipelineKey key{ shaderUUID, mode };
        auto it = m_Pipelines.find(key);
        if (it != m_Pipelines.end())
            return it->second.get();

        PipelineConfig config = m_ConfigFactory(mode);
        auto pipeline = std::make_unique<VKPipeline>(config, vertSpv, fragSpv, m_Layouts);
        VKPipeline* ptr = pipeline.get();
        m_Pipelines.emplace(key, std::move(pipeline));

        LH_CORE_INFO("Created pipeline variant: shader={} mode={}", shaderUUID.ToString(), static_cast<int>(mode));
        return ptr;
    }

    void PipelineManager::InvalidateShader(const UUID& shaderUUID)
    {
        size_t count = std::erase_if(m_Pipelines, [&](const auto& entry) {
            return entry.first.shaderUUID == shaderUUID;
        });

        if (count > 0)
            LH_CORE_INFO("Invalidated {} pipeline(s) for shader {}", count, shaderUUID.ToString());
    }

    void PipelineManager::Clear()
    {
        m_Pipelines.clear();
    }
}
