#pragma once

#include "luth/core/types/LuthTypes.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>

// Forward declare VMA types
typedef struct VmaAllocation_T* VmaAllocation;

namespace Luth
{
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
        // Sentinel for "texture is not registered in the bindless set" (depth, cubemap,
        // R32_Uint entity-ID, or BindTexture overflow). Distinct from slot 0 — which is
        // the reserved 1x1 white null-texture sampled on partially-bound misses.
        // Must NEVER reach the GPU: Set 1 has MAX_BINDLESS_RESOURCES slots, sampling at
        // UINT32_MAX is UB even with VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT (partial-bind
        // permits unbound slots, not out-of-range indices). Coerce to 0 at every SSBO write.
        static constexpr u32 INVALID_BINDLESS_SLOT = UINT32_MAX;

        void Init(VkDevice device);
        void Shutdown();

        // Returns a real slot, or INVALID_BINDLESS_SLOT when the pool is exhausted.
        // Thread-safe.
        u32 BindTexture(VkImageView view, VkSampler sampler);
        void UnbindTexture(u32 index);

        VkDescriptorSet GetSet() const { return m_DescriptorSet; }
        VkDescriptorSetLayout GetLayout() const { return m_Layout; }
        VkDescriptorPool GetPool() const { return m_Pool; }

    private:
        static constexpr u32 MAX_BINDLESS_RESOURCES = 16384;
        static constexpr u32 NULL_TEXTURE_SLOT      = 0;

        VkDevice m_Device = VK_NULL_HANDLE;
        VkDescriptorPool m_Pool = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
        VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;

        std::mutex m_Lock;
        std::vector<u32> m_FreeIndices; // LIFO: pop_back / push_back

        // Fallback 1x1 white texture for empty slots
        VkImage m_NullImage = VK_NULL_HANDLE;
        VkImageView m_NullImageView = VK_NULL_HANDLE;
        VkSampler m_NullSampler = VK_NULL_HANDLE;
        VmaAllocation m_NullAllocation = nullptr;

        void CreateNullTexture();
    };

    // Coerces a bindless index to a GPU-safe value. Use at every SSBO write that consumes
    // VKTexture::GetBindlessIndex() — the sentinel must never be sampled by a shader.
    inline u32 BindlessOrNull(u32 idx)
    {
        return idx == BindlessDescriptorSet::INVALID_BINDLESS_SLOT ? 0u : idx;
    }
}
