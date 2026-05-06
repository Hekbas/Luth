#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/core/FrameData.h"
#include "luth/jobs/SpinLock.h"
#include "luth/renderer/material/Material.h"
#include <vulkan/vulkan.h>
#include <array>
#include <vector>
#include <deque>
#include <memory>

namespace Luth
{
    // Material System — global SSBO for all active materials. Referenced by index in shaders.
    // Storage is allocated per-frame from GPUTaggedPageAllocator and the descriptor is
    // cycled across MAX_FRAMES_IN_FLIGHT slots — game frame K writes slot K%N, render
    // frame K-1 reads slot (K-1)%N → distinct slots, race-free, no UAB needed.

    class MaterialSystem
    {
    public:
        static void Init();
        static void Shutdown();

        // Registers a material and returns its slot index. Game-stage only.
        static u32 RegisterMaterial(std::shared_ptr<Material> material);

        // Unregisters (frees the slot). Game-stage only.
        static void UnregisterMaterial(u32 index);

        // Uploads all live materials to a fresh allocator-returned region and writes
        // the GAME-frame's Set 2 descriptor slot. Called once per game stage from RenderSnapshot::Capture.
        static void Update(VkCommandBuffer cmd);

        // Returns the descriptor set for the given slot. Bind sites pass
        // `Renderer::GetFrameData()->GetRenderFrameIndex() % MAX_FRAMES_IN_FLIGHT`.
        static VkDescriptorSet GetDescriptorSet(u32 slot);
        static VkDescriptorSetLayout GetDescriptorSetLayout();

    private:
        static constexpr u32 MAX_MATERIALS = 16384;
        static constexpr u32 MATERIAL_SIZE = sizeof(GPUMaterialData);

        struct MaterialSlot
        {
            std::shared_ptr<Material> material;
        };

        static void CreateDescriptors();

        static VkDescriptorPool      m_DescriptorPool;
        static VkDescriptorSetLayout m_DescriptorSetLayout;
        static std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_DescriptorSets;

        static std::vector<MaterialSlot> m_Slots;
        static std::deque<u32> m_FreeIndices;
        // V1: SpinLock — std::mutex retired post-gpu-tagged-heap when per-frame
        // upload moved off the lock. Update still holds for ~25us iterating slots,
        // safe today because game-stage callers serialize via RenderSnapshot::Capture.
        static Luth::SpinLock m_Lock;
    };
}
