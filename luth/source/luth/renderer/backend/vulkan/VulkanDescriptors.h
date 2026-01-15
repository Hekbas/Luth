#pragma once

#include "luth/core/LuthTypes.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <deque>
#include <mutex>

// Forward declare VMA types
typedef struct VmaAllocation_T* VmaAllocation;

namespace Luth
{
    // Manages a pool of descriptors, growing as needed
    class DescriptorAllocator
    {
    public:
        void Init(VkDevice device);
        void Shutdown();

        bool Allocate(VkDescriptorSetLayout layout, VkDescriptorSet& outSet);
        void Reset(); // Resets all pools (called at start of frame)

    private:
        VkDescriptorPool CreatePool(u32 count, VkDescriptorPoolCreateFlags flags);
        VkDescriptorPool GetPool();

        VkDevice m_Device = VK_NULL_HANDLE;
        VkDescriptorPool m_CurrentPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> m_UsedPools;
        std::vector<VkDescriptorPool> m_FreePools;
    };

    // Caches descriptor set layouts to avoid duplicate creation
    class DescriptorLayoutCache
    {
    public:
        void Init(VkDevice device);
        void Shutdown();

        VkDescriptorSetLayout CreateDescriptorLayout(VkDescriptorSetLayoutCreateInfo* info);

    private:
        struct LayoutInfo {
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            bool operator==(const LayoutInfo& other) const;
            size_t hash() const;
        };

        struct LayoutHash {
            size_t operator()(const LayoutInfo& k) const { return k.hash(); }
        };

        VkDevice m_Device = VK_NULL_HANDLE;
        std::unordered_map<LayoutInfo, VkDescriptorSetLayout, LayoutHash> m_LayoutCache;
    };

    // Manages the global bindless texture array (Set 0)
    // Supports VK_EXT_descriptor_indexing
    class BindlessDescriptorSet
    {
    public:
        void Init(VkDevice device);
        void Shutdown();

        // Returns the index in the global array
        // Thread-safe
        u32 BindTexture(VkImageView view, VkSampler sampler);
        void UnbindTexture(u32 index);

        VkDescriptorSet GetSet() const { return m_DescriptorSet; }
        VkDescriptorSetLayout GetLayout() const { return m_Layout; }
        VkDescriptorPool GetPool() const { return m_Pool; }

    private:
        static constexpr u32 MAX_BINDLESS_RESOURCES = 16384;

        VkDevice m_Device = VK_NULL_HANDLE;
        VkDescriptorPool m_Pool = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
        VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;

        std::mutex m_Lock;
        std::deque<u32> m_FreeIndices;
        
        // Fallback 1x1 white texture for empty slots
        VkImage m_NullImage = VK_NULL_HANDLE;
        VkImageView m_NullImageView = VK_NULL_HANDLE;
        VkSampler m_NullSampler = VK_NULL_HANDLE;
        VmaAllocation m_NullAllocation = nullptr;

        void CreateNullTexture();
    };
}
