#include "luthpch.h"
#include "PipelineManager.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/core/diagnostics/Log.h"

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

    VKPipeline* PipelineManager::GetOrCreate(const UUID& shaderUUID, Material::RenderMode mode, Material::CullMode cullMode,
                                              VkPolygonMode polygonMode,
                                              const std::vector<u32>& vertSpv, const std::vector<u32>& fragSpv)
    {
        PipelineKey key{ shaderUUID, mode, cullMode, polygonMode };
        auto it = m_Pipelines.find(key);
        if (it != m_Pipelines.end())
            return it->second.get();

        PipelineConfig config = m_ConfigFactory(mode, cullMode, polygonMode);
        auto pipeline = std::make_unique<VKPipeline>(config, vertSpv, fragSpv, m_Layouts);
        VKPipeline* ptr = pipeline.get();
        m_Pipelines.emplace(key, std::move(pipeline));

        LH_CORE_INFO("Created pipeline variant: shader={} mode={} cull={} poly={}", shaderUUID.ToString(), static_cast<int>(mode), static_cast<int>(cullMode), static_cast<int>(polygonMode));
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

    void PipelineManager::DeferredInvalidateShader(const UUID& shaderUUID)
    {
        size_t count = 0;
        for (auto it = m_Pipelines.begin(); it != m_Pipelines.end(); )
        {
            if (it->first.shaderUUID == shaderUUID)
            {
                if (auto* raw = it->second.release(); raw)
                    VulkanContext::Get().PushDeletion([raw]() { delete raw; });
                it = m_Pipelines.erase(it);
                ++count;
            }
            else
            {
                ++it;
            }
        }
        if (count > 0)
            LH_CORE_INFO("Deferred-invalidated {} pipeline(s) for shader {}", count, shaderUUID.ToString());
    }

    void PipelineManager::DeferredClear()
    {
        for (auto& [key, ptr] : m_Pipelines)
        {
            if (auto* raw = ptr.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        }
        m_Pipelines.clear();
    }
}
