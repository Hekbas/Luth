#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/jobs/SpinLock.h"
#include "luth/renderer/material/Material.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <deque>
#include <memory>

namespace Luth
{
    // Material System — global SSBO for all active materials. Referenced by index in shaders.
    // Storage is allocated per-frame from GPUTaggedPageAllocator and the descriptor is
    // rewritten at start of each game stage (Set 2). Slot indices are 0-based.

    class MaterialSystem
    {
    public:
        static void Init();
        static void Shutdown();

        // Registers a material and returns its slot index. Game-stage only.
        static u32 RegisterMaterial(std::shared_ptr<Material> material);

        // Unregisters (frees the slot). Game-stage only.
        static void UnregisterMaterial(u32 index);

        // Uploads all live materials to a fresh allocator-returned region and rewrites
        // the Set 2 descriptor. Called once per game stage from RenderSnapshot::Capture.
        static void Update(VkCommandBuffer cmd);

        static VkDescriptorSet GetDescriptorSet();
        static VkDescriptorSetLayout GetDescriptorSetLayout();

    private:
        static constexpr u32 MAX_MATERIALS = 16384;
        static constexpr u32 MATERIAL_SIZE = sizeof(GPUMaterialData);

        struct MaterialSlot
        {
            std::shared_ptr<Material> material;
        };

        static void CreateDescriptors();

        static VkDescriptorPool m_DescriptorPool;
        static VkDescriptorSetLayout m_DescriptorSetLayout;
        static VkDescriptorSet m_DescriptorSet;

        static std::vector<MaterialSlot> m_Slots;
        static std::deque<u32> m_FreeIndices;
        // V1: SpinLock — std::mutex retired post-gpu-tagged-heap when per-frame
        // upload moved off the lock. Update still holds for ~25us iterating slots,
        // safe today because game-stage callers serialize via RenderSnapshot::Capture.
        static Luth::SpinLock m_Lock;
    };
}
