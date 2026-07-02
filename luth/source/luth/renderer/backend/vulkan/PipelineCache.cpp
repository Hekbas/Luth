#include "luthpch.h"
#include <atomic>
#include "PipelineCache.h"
#include "VulkanContext.h"
#include "luth/resources/FileSystem.h"

namespace Luth
{
    VkPipelineCache PipelineCache::s_Cache = VK_NULL_HANDLE;

    static constexpr const char* k_CacheRelPath = "Library/PipelineCache.bin";

    void PipelineCache::Init()
    {
        LH_PROFILE_FUNCTION();

        VkDevice device = VulkanContext::Get().GetDevice();

        VkPipelineCacheCreateInfo createInfo{};
        createInfo.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        createInfo.initialDataSize = 0;
        createInfo.pInitialData    = nullptr;

        if (vkCreatePipelineCache(device, &createInfo, nullptr, &s_Cache) != VK_SUCCESS)
        {
            LH_LOG(Renderer, error, "Failed to create pipeline cache");
            s_Cache = VK_NULL_HANDLE;
            return;
        }

        LH_LOG(Renderer, info, "Pipeline cache created (empty)");
    }

    void PipelineCache::LoadFromProject()
    {
        LH_PROFILE_FUNCTION();

        if (s_Cache == VK_NULL_HANDLE) return;
        if (!FileSystem::HasProject()) return;

        fs::path cachePath = FileSystem::ProjectPath(k_CacheRelPath);
        if (!fs::exists(cachePath)) return;

        std::ifstream file(cachePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            LH_LOG(Renderer, warn, "Failed to open pipeline cache: {}", cachePath.string());
            return;
        }

        size_t fileSize = static_cast<size_t>(file.tellg());
        if (fileSize == 0) return;

        std::vector<char> cacheData(fileSize);
        file.seekg(0);
        file.read(cacheData.data(), fileSize);
        file.close();

        VkDevice device = VulkanContext::Get().GetDevice();

        // Create a temp cache from the file blob, then merge into the live cache.
        VkPipelineCacheCreateInfo createInfo{};
        createInfo.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        createInfo.initialDataSize = cacheData.size();
        createInfo.pInitialData    = cacheData.data();

        VkPipelineCache temp = VK_NULL_HANDLE;
        if (vkCreatePipelineCache(device, &createInfo, nullptr, &temp) != VK_SUCCESS)
        {
            LH_LOG(Renderer, warn, "Failed to deserialize pipeline cache from {}", cachePath.string());
            return;
        }

        if (vkMergePipelineCaches(device, s_Cache, 1, &temp) != VK_SUCCESS)
            LH_LOG(Renderer, warn, "Failed to merge pipeline cache from {}", cachePath.string());
        else
            LH_LOG(Renderer, info, "Pipeline cache loaded ({} bytes) from {}", fileSize, cachePath.string());

        vkDestroyPipelineCache(device, temp, nullptr);
    }

    void PipelineCache::SaveToProject()
    {
        LH_PROFILE_FUNCTION();

        if (s_Cache == VK_NULL_HANDLE) return;
        if (!FileSystem::HasProject()) return;

        VkDevice device = VulkanContext::Get().GetDevice();

        size_t dataSize = 0;
        vkGetPipelineCacheData(device, s_Cache, &dataSize, nullptr);
        if (dataSize == 0) return;

        std::vector<char> cacheData(dataSize);
        vkGetPipelineCacheData(device, s_Cache, &dataSize, cacheData.data());

        fs::path cachePath = FileSystem::ProjectPath(k_CacheRelPath);
        FileSystem::CreateDirectories(cachePath.parent_path());

        std::ofstream file(cachePath, std::ios::binary);
        if (!file.is_open())
        {
            LH_LOG(Renderer, error, "Failed to write pipeline cache to {}", cachePath.string());
            return;
        }

        file.write(cacheData.data(), dataSize);
        LH_LOG(Renderer, info, "Pipeline cache saved ({} bytes) to {}", dataSize, cachePath.string());
    }

    void PipelineCache::Shutdown()
    {
        if (s_Cache == VK_NULL_HANDLE) return;
        VkDevice device = VulkanContext::Get().GetDevice();
        vkDestroyPipelineCache(device, s_Cache, nullptr);
        s_Cache = VK_NULL_HANDLE;
    }

    // ---- Compile-time instrumentation ----
    // Reset each frame by App's profiling plots (ConsumeFrameStats). Recorded from pipeline ctors
    // that may run on worker fibers during graph recording, so all access is atomic.
    static std::atomic<u32> s_CompileCount{ 0 };
    static std::atomic<u64> s_CompileTotalNs{ 0 };
    static std::atomic<u64> s_CompileMaxNs{ 0 };

    void PipelineCache::RecordCompile(f64 ms)
    {
        const u64 ns = static_cast<u64>(ms * 1.0e6);
        s_CompileCount.fetch_add(1, std::memory_order_relaxed);
        s_CompileTotalNs.fetch_add(ns, std::memory_order_relaxed);
        for (u64 prev = s_CompileMaxNs.load(std::memory_order_relaxed);
             ns > prev && !s_CompileMaxNs.compare_exchange_weak(prev, ns, std::memory_order_relaxed); ) {}

        // A >1ms create is a cold miss worth flagging on the Tracy timeline (runtime stutter).
        if (ms > 1.0)
            LH_PROFILE_MESSAGE_COLOR(("PSO compile " + std::to_string(ms) + " ms").c_str(), 0xFFFF8000);
    }

    PipelineCache::CompileStats PipelineCache::ConsumeFrameStats()
    {
        CompileStats s;
        s.count   = s_CompileCount.exchange(0, std::memory_order_relaxed);
        s.totalMs = static_cast<f64>(s_CompileTotalNs.exchange(0, std::memory_order_relaxed)) / 1.0e6;
        s.maxMs   = static_cast<f64>(s_CompileMaxNs.exchange(0, std::memory_order_relaxed)) / 1.0e6;
        return s;
    }
}
