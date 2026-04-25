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

        // Uploads dirty materials to the GPU. Called once per frame.
        static void Update(VkCommandBuffer cmd);

        static VkDescriptorSet GetDescriptorSet(); // Returns the set containing the Material Buffer
        static VkDescriptorSetLayout GetDescriptorSetLayout();

    private:
        static constexpr u32 MAX_MATERIALS = 16384;
        static constexpr u32 MATERIAL_SIZE = sizeof(GPUMaterialData);

        struct MaterialSlot
        {
            std::shared_ptr<Material> material;
            bool dirty = false;
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
