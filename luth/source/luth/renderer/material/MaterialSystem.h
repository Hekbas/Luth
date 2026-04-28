#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/renderer/material/Material.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <deque>
#include <mutex>
#include <memory>

// Forward declare VMA types
typedef struct VmaAllocation_T* VmaAllocation;

namespace Luth
{
    // Material System — global SSBO for all active materials. Referenced by index in shaders.
    
    class MaterialSystem
    {
    public:
        static void Init();
        static void Shutdown();

        // Registers a material and returns its index in the global buffer.
        // If the material is already registered, updates it.
        static u32 RegisterMaterial(std::shared_ptr<Material> material);
        
        // Unregisters a material (frees the slot).
        static void UnregisterMaterial(u32 index);

        // Uploads dirty materials to the GPU. Called once per frame from
        // the game stage. gameSlot selects the active ring slice of the
        // persistent material SSBO (GPU frame N's consumer); sourced from
        // FrameData::GameSlot() at the call site.
        static void Update(VkCommandBuffer cmd, u32 gameSlot);

        static VkDescriptorSet GetDescriptorSet(); // Returns the set containing the Material Buffer
        static VkDescriptorSetLayout GetDescriptorSetLayout();

        // Public so writers (BuildGPUObjectBuffer) can encode the active ring
        // slice into obj.materialIndex: gameSlot * MAX_MATERIALS + matSlot.
        static constexpr u32 MAX_MATERIALS = 16384;

    private:
        static constexpr u32 MATERIAL_SIZE = sizeof(GPUMaterialData);

        struct MaterialSlot
        {
            std::shared_ptr<Material> material;
            // Countdown of remaining ring slices to upload after a change is detected.
            // Initialized to MAX_FRAMES_IN_FLIGHT on register / change-detected; the
            // game-stage Update() writes the active gameSlot slice and decrements,
            // so a single mutation propagates to all slices over consecutive frames.
            u8 dirtyFramesRemaining = 0;
        };

        static void CreateBuffer();
        static void CreateDescriptors();

        static VkBuffer m_Buffer;
        static VmaAllocation m_Allocation;
        static void* m_MappedData;

        static VkDescriptorPool m_DescriptorPool;
        static VkDescriptorSetLayout m_DescriptorSetLayout;
        static VkDescriptorSet m_DescriptorSet;

        static std::vector<MaterialSlot> m_Slots;
        static std::deque<u32> m_FreeIndices;
        static std::mutex m_Lock;
    };
}
