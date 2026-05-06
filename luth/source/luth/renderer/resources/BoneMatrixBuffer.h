#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/core/FrameData.h"
#include "luth/jobs/SpinLock.h"
#include "luth/renderer/resources/Skeleton.h"
#include <vulkan/vulkan.h>
#include <array>
#include <deque>

namespace Luth
{
    // ===================================================================================
    // Bone Matrix Buffer (Global Bone SSBO for GPU Skinning)
    // ===================================================================================
    // Slot-based block allocator (per-entity stable offsets baked into obj.boneOffset).
    // Bone data accumulates into a CPU staging buffer during the game stage; Update()
    // copies the staging into a fresh per-frame GPU region (GPUTaggedPageAllocator) and
    // writes the GAME-frame's descriptor slot. Bind sites read the RENDER-frame's slot
    // — game writes K, render reads K-1 → distinct slots, race-free.
    // invariant: slot count = MAX_FRAMES_IN_FLIGHT; the cycling decouples write/read by
    // frame so the layout no longer needs UPDATE_AFTER_BIND.

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
        // writes the game-frame's descriptor slot. Called once per game stage.
        static void Update();

        // Returns the descriptor set for the given slot. Call sites pass
        // `Renderer::GetFrameData()->GetRenderFrameIndex() % MAX_FRAMES_IN_FLIGHT`
        // (or, for FrameDebugger replay, the captured slot pinned at capture time).
        static VkDescriptorSet GetDescriptorSet(u32 slot);
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

        static VkDescriptorPool      m_DescriptorPool;
        static VkDescriptorSetLayout m_DescriptorSetLayout;
        static std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_DescriptorSets;

        static std::deque<u32> m_FreeBlocks; // Block indices (0..MAX_SKINNED_ENTITIES-1)
        // V1: SpinLock — protects only AllocateBlock/FreeBlock (O(1) deque ops).
        // UploadBones / Update don't take this lock (CPU staging is shared, writes are disjoint).
        static Luth::SpinLock m_Lock;
    };
}
