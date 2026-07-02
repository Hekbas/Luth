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

    // Manages the global bindless descriptor set (Set 1):
    //   binding 0: combined image-samplers (16384 slots; the per-texture sampler pattern)
    //   binding 1: pure samplers (32 slots; canonical samplers at the front, ad-hoc after)
    // Supports VK_EXT_descriptor_indexing.
    class BindlessDescriptorSet
    {
    public:
        // Sentinel for "texture is not registered in the bindless set" (depth, cubemap, R32_Uint entity-ID,
        // or BindTexture overflow). Distinct from slot 0, the reserved 1x1 white null-texture sampled on
        // partially-bound misses. Must NEVER reach the GPU: binding 0 has MAX_BINDLESS_RESOURCES slots, and
        // sampling at UINT32_MAX is UB even with VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT (partial-bind
        // permits unbound slots, not out-of-range indices). Coerce to 0 at every SSBO write.
        static constexpr u32 INVALID_BINDLESS_SLOT = UINT32_MAX;

        // Pre-populated samplers at fixed slots of binding 1. Future shader paths reference
        // them by index (cast to u32). LIFO sampler allocation starts after the canonical block.
        enum class CanonicalSampler : u32 {
            LinearRepeatAnisoMip = 0,   // default for material color textures
            LinearClampAnisoMip  = 1,   // IBL, environment samples
            NearestRepeatNoMip   = 2,   // stylized / data textures
            NearestClampNoMip    = 3,   // masks, lookup tables
            Count
        };
        static constexpr u32 NUM_CANONICAL_SAMPLERS = static_cast<u32>(CanonicalSampler::Count);
        static constexpr u32 GetCanonicalSamplerIndex(CanonicalSampler s) { return static_cast<u32>(s); }

        void Init(VkDevice device);
        void Shutdown();

        // Returns a real slot, or INVALID_BINDLESS_SLOT when the pool is exhausted. Thread-safe.
        u32 BindTexture(VkImageView view, VkSampler sampler);
        void UnbindTexture(u32 index);

        // Same allocation model for ad-hoc samplers (slot index lives in binding 1). The
        // canonical block (0..NUM_CANONICAL_SAMPLERS-1) is not vended through BindSampler.
        u32 BindSampler(VkSampler sampler);
        void UnbindSampler(u32 index);

        VkDescriptorSet GetSet() const { return m_DescriptorSet; }
        VkDescriptorSetLayout GetLayout() const { return m_Layout; }
        VkDescriptorPool GetPool() const { return m_Pool; }

    private:
        static constexpr u32 MAX_BINDLESS_RESOURCES = 16384;
        static constexpr u32 MAX_BINDLESS_SAMPLERS  = 32;
        static constexpr u32 NULL_TEXTURE_SLOT      = 0;

        VkDevice m_Device = VK_NULL_HANDLE;
        VkDescriptorPool m_Pool = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
        VkDescriptorSet m_DescriptorSet = VK_NULL_HANDLE;

        std::mutex m_Lock;
        std::vector<u32> m_FreeIndices;        // binding 0 LIFO
        std::vector<u32> m_FreeSamplerIndices; // binding 1 LIFO (post-canonical block)

        // Fallback 1x1 white texture for empty binding-0 slots
        VkImage m_NullImage = VK_NULL_HANDLE;
        VkImageView m_NullImageView = VK_NULL_HANDLE;
        VkSampler m_NullSampler = VK_NULL_HANDLE;
        VmaAllocation m_NullAllocation = nullptr;

        // Canonical sampler objects (slot index == enum value). Created in Init, destroyed in Shutdown.
        VkSampler m_CanonicalSamplers[NUM_CANONICAL_SAMPLERS]{};

        void CreateNullTexture();
        void CreateCanonicalSamplers();
        void WriteSamplerSlot(u32 index, VkSampler sampler);
    };

    // Coerces a bindless index to a GPU-safe value. Use at every SSBO write that consumes
    // VKTexture::GetBindlessIndex(); the sentinel must never be sampled by a shader.
    inline u32 BindlessOrNull(u32 idx)
    {
        return idx == BindlessDescriptorSet::INVALID_BINDLESS_SLOT ? 0u : idx;
    }
}
