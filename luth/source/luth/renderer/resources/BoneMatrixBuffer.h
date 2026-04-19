#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/animation/Skeleton.h"
#include <vulkan/vulkan.h>
#include <deque>
#include <mutex>

// Forward declare VMA types
typedef struct VmaAllocation_T* VmaAllocation;

namespace Luth
{
    // ===================================================================================
    // Bone Matrix Buffer (Global Bone SSBO for GPU Skinning)
    // ===================================================================================
    // Manages a persistently-mapped SSBO containing bone matrices for all skinned entities.
    // Bones are referenced by base offset in the vertex shader push constants.
    // Follows the MaterialSystem pattern: static singleton, slot-based allocation.

    class BoneMatrixBuffer
    {
    public:
        static void Init();
        static void Shutdown();

        // Allocates a block of MAX_BONES matrices. Returns the base index into the SSBO.
        static u32 AllocateBlock();

        // Frees a previously allocated block.
        static void FreeBlock(u32 baseIndex);

        // Uploads bone matrices for an entity. baseIndex from AllocateBlock().
        static void UploadBones(u32 baseIndex, const Mat4* matrices, u32 count);

        static VkDescriptorSet GetDescriptorSet();
        static VkDescriptorSetLayout GetDescriptorSetLayout();

    private:
        static constexpr u32 MAX_SKINNED_ENTITIES = 128;
        static constexpr u32 BONES_PER_ENTITY = MAX_BONES; // 256
        static constexpr u32 TOTAL_MATRICES = MAX_SKINNED_ENTITIES * BONES_PER_ENTITY; // 32768
        static constexpr u32 MATRIX_SIZE = sizeof(Mat4);   // 64 bytes
        static constexpr u32 BUFFER_SIZE = TOTAL_MATRICES * MATRIX_SIZE; // 2 MB

        static void CreateBuffer();
        static void CreateDescriptors();

        static VkBuffer m_Buffer;
        static VmaAllocation m_Allocation;
        static void* m_MappedData;

        static VkDescriptorPool m_DescriptorPool;
        static VkDescriptorSetLayout m_DescriptorSetLayout;
        static VkDescriptorSet m_DescriptorSet;

        static std::deque<u32> m_FreeBlocks; // Block indices (0..MAX_SKINNED_ENTITIES-1)
        static std::mutex m_Lock;
    };
}
