#include "luthpch.h"
#include "BoneMatrixBuffer.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/core/FrameData.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/memory/MemoryMacros.h"

namespace Luth
{
    byte* BoneMatrixBuffer::m_CpuScratch = nullptr;

    VkDescriptorPool      BoneMatrixBuffer::m_DescriptorPool      = VK_NULL_HANDLE;
    VkDescriptorSetLayout BoneMatrixBuffer::m_DescriptorSetLayout = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> BoneMatrixBuffer::m_DescriptorSets{};

    std::deque<u32> BoneMatrixBuffer::m_FreeBlocks;
    Luth::SpinLock BoneMatrixBuffer::m_Lock;

    void BoneMatrixBuffer::Init()
    {
        CreateDescriptors();

        m_FreeBlocks.clear();
        for (u32 i = 0; i < MAX_SKINNED_ENTITIES; ++i)
            m_FreeBlocks.push_back(i);

        // Persistent CPU staging — game-stage writers fill this; Update() copies to GPU.
        m_CpuScratch = static_cast<byte*>(LH_ALLOC(Memory::Category::Rendering, BUFFER_SIZE));

        // Identity-fill so unallocated blocks render bind pose (used to be done on the
        // mapped GPU buffer; the CPU scratch now plays that role and propagates each frame).
        Mat4 identity(1.0f);
        for (u32 i = 0; i < TOTAL_MATRICES; ++i)
            memcpy(m_CpuScratch + i * MATRIX_SIZE, &identity, MATRIX_SIZE);

        LH_CORE_INFO("BoneMatrixBuffer initialized ({0} entities x {1} bones = {2} KB)",
            MAX_SKINNED_ENTITIES, BONES_PER_ENTITY, BUFFER_SIZE / 1024);
    }

    void BoneMatrixBuffer::Shutdown()
    {
        m_FreeBlocks.clear();

        VkDevice device = VulkanContext::Get().GetDevice();
        vkDestroyDescriptorPool(device, m_DescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, m_DescriptorSetLayout, nullptr);

        if (m_CpuScratch)
        {
            LH_FREE(Memory::Category::Rendering, m_CpuScratch, BUFFER_SIZE);
            m_CpuScratch = nullptr;
        }
    }

    u32 BoneMatrixBuffer::AllocateBlock()
    {
        // Free-list mutation must run on the game stage so the buffer slot
        // a render-stage draw references (via snapshot.boneOffset) is stable.
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "BoneMatrixBuffer::AllocateBlock must run on the game stage");
        SpinLockGuard lock(m_Lock);

        if (m_FreeBlocks.empty())
        {
            LH_CORE_ERROR("BoneMatrixBuffer: Out of blocks! (max {0})", MAX_SKINNED_ENTITIES);
            return 0;
        }

        u32 blockIndex = m_FreeBlocks.front();
        m_FreeBlocks.pop_front();
        return blockIndex * BONES_PER_ENTITY;
    }

    void BoneMatrixBuffer::FreeBlock(u32 baseIndex)
    {
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "BoneMatrixBuffer::FreeBlock must run on the game stage");
        SpinLockGuard lock(m_Lock);

        u32 blockIndex = baseIndex / BONES_PER_ENTITY;
        if (blockIndex >= MAX_SKINNED_ENTITIES) return;
        m_FreeBlocks.push_back(blockIndex);
    }

    void BoneMatrixBuffer::UploadBones(u32 baseIndex, const Mat4* matrices, u32 count)
    {
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "BoneMatrixBuffer::UploadBones must run on the game stage");
        if (!m_CpuScratch) return;
        if (baseIndex + count > TOTAL_MATRICES) return;

        // Per-entity baseIndex is unique → concurrent fiber writes hit disjoint ranges.
        // No lock needed (same property as the v2.8.9 mapped-buffer write).
        memcpy(m_CpuScratch + baseIndex * MATRIX_SIZE, matrices, count * MATRIX_SIZE);
    }

    void BoneMatrixBuffer::Update()
    {
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "BoneMatrixBuffer::Update must run on the game stage");
        if (!m_CpuScratch) return;

        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return;
        const u64 gameFrame = Renderer::GetFrameData()->GetFrameIndex();
        jobCtx->GpuCache.CurrentTag = static_cast<u32>(gameFrame);

        auto& heap = Memory::GPUTaggedPageAllocator::Get();
        Memory::GPUSubRegion region = heap.Allocate(jobCtx->GpuCache, BUFFER_SIZE, 16);
        if (!region.buffer) return;

        memcpy(region.mappedPtr, m_CpuScratch, BUFFER_SIZE);
        heap.FlushRegion(region);

        // Write the GAME-frame slot. Render stage of frame K-1 reads slot (K-1)%N
        // while we're writing slot K%N — distinct slots, no race, no UAB needed.
        const u32 slot = static_cast<u32>(gameFrame) % MAX_FRAMES_IN_FLIGHT;

        VkDescriptorBufferInfo bi{};
        bi.buffer = region.buffer;
        bi.offset = region.offset;
        bi.range  = region.size;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_DescriptorSets[slot];
        write.dstBinding      = 0;
        write.dstArrayElement = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo     = &bi;
        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);
    }

    VkDescriptorSet BoneMatrixBuffer::GetDescriptorSet(u32 slot)
    {
        return m_DescriptorSets[slot % MAX_FRAMES_IN_FLIGHT];
    }

    VkDescriptorSetLayout BoneMatrixBuffer::GetDescriptorSetLayout()
    {
        return m_DescriptorSetLayout;
    }

    void BoneMatrixBuffer::CreateDescriptors()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Set 4 layout — non-UAB. Cycling provides write/read isolation by frame slot;
        // each Update() writes a slot the render stage isn't currently consuming.
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &binding;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_DescriptorSetLayout);

        // Pool sized for MAX_FRAMES_IN_FLIGHT sets; one storage-buffer descriptor each.
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = MAX_FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets       = MAX_FRAMES_IN_FLIGHT;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_DescriptorPool);

        // Allocate all N slots. Each per-game-stage Update() writes its own slot.
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
        layouts.fill(m_DescriptorSetLayout);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_DescriptorPool;
        allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        allocInfo.pSetLayouts        = layouts.data();
        vkAllocateDescriptorSets(device, &allocInfo, m_DescriptorSets.data());
    }
}
