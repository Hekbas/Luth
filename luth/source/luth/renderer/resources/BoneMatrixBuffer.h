#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/renderer/resources/Skeleton.h"
#include <vulkan/vulkan.h>
#include <deque>
#include <mutex>

namespace Luth
{
    // ===================================================================================
    // Bone Matrix Buffer (Global Bone SSBO for GPU Skinning)
    // ===================================================================================
    // Slot-based block allocator (per-entity stable offsets baked into obj.boneOffset).
    // Bone data accumulates into a CPU staging buffer during the game stage; Update()
    // copies the staging into a fresh per-frame GPU region (GPUTaggedPageAllocator) and
    // rebinds Set 4. Follows the same per-frame upload pattern as MaterialSystem.

    class BoneMatrixBuffer
    {
    public:
        static void Init();
        static void Shutdown();

        // Allocates a block of MAX_BONES matrices. Returns the base matrix index.
        static u32 AllocateBlock();

        // Frees a previously allocated block.
        static void FreeBlock(u32 baseIndex);

        // Stages bone matrices for an entity. baseIndex from AllocateBlock().
        // Multiple game-stage fibers may call concurrently — disjoint ranges per entity.
        static void UploadBones(u32 baseIndex, const Mat4* matrices, u32 count);

        // Allocates this frame's GPU region, copies CPU staging in, flushes, and
        // rewrites Set 4 descriptor. Called once per game stage from RenderSnapshot.
        static void Update();

        static VkDescriptorSet GetDescriptorSet();
        static VkDescriptorSetLayout GetDescriptorSetLayout();

    private:
        static constexpr u32 MAX_SKINNED_ENTITIES = 128;
        static constexpr u32 BONES_PER_ENTITY = MAX_BONES; // 256
        static constexpr u32 TOTAL_MATRICES = MAX_SKINNED_ENTITIES * BONES_PER_ENTITY; // 32768
        static constexpr u32 MATRIX_SIZE = sizeof(Mat4);   // 64 bytes
        static constexpr u32 BUFFER_SIZE = TOTAL_MATRICES * MATRIX_SIZE; // 2 MB

        static void CreateDescriptors();

        // CPU staging — game-stage writers fill this; Update() copies it to GPU.
        // Initialized to identity at Init so unallocated blocks render bind pose.
        static byte* m_CpuScratch;

        static VkDescriptorPool m_DescriptorPool;
        static VkDescriptorSetLayout m_DescriptorSetLayout;
        static VkDescriptorSet m_DescriptorSet;

        static std::deque<u32> m_FreeBlocks; // Block indices (0..MAX_SKINNED_ENTITIES-1)
        static std::mutex m_Lock;
    };
}
