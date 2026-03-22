#include "luthpch.h"
#include "PipelineCache.h"
#include "VulkanContext.h"
#include "luth/resources/FileSystem.h"

namespace Luth
{
    VkPipelineCache PipelineCache::s_Cache = VK_NULL_HANDLE;

    void PipelineCache::Init()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        fs::path cachePath = FileSystem::ProjectPath("cache/pipeline.bin");

        std::vector<char> cacheData;
        if (fs::exists(cachePath))
        {
            std::ifstream file(cachePath, std::ios::binary | std::ios::ate);
            if (file.is_open())
            {
                size_t fileSize = static_cast<size_t>(file.tellg());
                cacheData.resize(fileSize);
                file.seekg(0);
                file.read(cacheData.data(), fileSize);
                file.close();
                LH_CORE_INFO("Pipeline cache loaded ({} bytes)", fileSize);
            }
        }

        VkPipelineCacheCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        createInfo.initialDataSize = cacheData.size();
        createInfo.pInitialData = cacheData.empty() ? nullptr : cacheData.data();

        if (vkCreatePipelineCache(device, &createInfo, nullptr, &s_Cache) != VK_SUCCESS)
        {
            LH_CORE_ERROR("Failed to create pipeline cache");
            s_Cache = VK_NULL_HANDLE;
            return;
        }

        if (cacheData.empty())
            LH_CORE_INFO("Pipeline cache created (empty)");
    }

    void PipelineCache::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        if (s_Cache == VK_NULL_HANDLE) return;

        // Retrieve cache data
        size_t dataSize = 0;
        vkGetPipelineCacheData(device, s_Cache, &dataSize, nullptr);

        if (dataSize > 0)
        {
            std::vector<char> cacheData(dataSize);
            vkGetPipelineCacheData(device, s_Cache, &dataSize, cacheData.data());

            fs::path cachePath = FileSystem::ProjectPath("cache/pipeline.bin");
            FileSystem::CreateDirectories(cachePath.parent_path());

            std::ofstream file(cachePath, std::ios::binary);
            if (file.is_open())
            {
                file.write(cacheData.data(), dataSize);
                file.close();
                LH_CORE_INFO("Pipeline cache saved ({} bytes)", dataSize);
            }
            else
            {
                LH_CORE_ERROR("Failed to write pipeline cache to disk");
            }
        }

        vkDestroyPipelineCache(device, s_Cache, nullptr);
        s_Cache = VK_NULL_HANDLE;
    }
}
