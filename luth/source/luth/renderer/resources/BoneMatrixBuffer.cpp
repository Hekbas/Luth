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
    byte* BoneMatrixBuffer::m_CpuScratch     = nullptr;
    byte* BoneMatrixBuffer::m_PrevCpuScratch = nullptr;

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

        // Persistent CPU staging — game-stage writers fill m_CpuScratch; Update() copies it AND
        // m_PrevCpuScratch (frame N-1 snapshot) into a dual-region GPU SSBO. Identity-fill both
        // so unallocated blocks render bind pose for current AND zero motion for previous.
        m_CpuScratch     = static_cast<byte*>(LH_ALLOC(Memory::Category::Rendering, BUFFER_SIZE));
        m_PrevCpuScratch = static_cast<byte*>(LH_ALLOC(Memory::Category::Rendering, BUFFER_SIZE));

        Mat4 identity(1.0f);
        for (u32 i = 0; i < TOTAL_MATRICES; ++i)
        {
            memcpy(m_CpuScratch     + i * MATRIX_SIZE, &identity, MATRIX_SIZE);
            memcpy(m_PrevCpuScratch + i * MATRIX_SIZE, &identity, MATRIX_SIZE);
        }

        LH_CORE_INFO("BoneMatrixBuffer initialized ({0} entities x {1} bones x 2 halves = {2} KB)",
            MAX_SKINNED_ENTITIES, BONES_PER_ENTITY, (2 * BUFFER_SIZE) / 1024);
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
        if (m_PrevCpuScratch)
        {
            LH_FREE(Memory::Category::Rendering, m_PrevCpuScratch, BUFFER_SIZE);
            m_PrevCpuScratch = nullptr;
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

        // Per-entity baseIndex is unique → concurrent fiber writes hit disjoint ranges,
        // so no lock is needed against other UploadBones callers on the same frame.
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
        // Tag gameFrame+1: this region is written game-side but read render-side one iteration later,
        // so a gameFrame tag would retire it the frame its GPU read completes (0 margin). +1 trails
        // GPU consumption by a frame. invariant: descriptor slot below stays keyed to gameFrame. see arch/memory.md
        jobCtx->GpuCache.CurrentTag = static_cast<u32>(gameFrame + 1);

        auto& heap = Memory::GPUTaggedPageAllocator::Get();
        // Dual region: first half = current bones (m_CpuScratch), second half = previous bones
        // (m_PrevCpuScratch, snapshotted at end of last frame's Update). Slim G-buffer skinned
        // shader reads `bones[boneOffset + i]` and `bones[boneOffset + PREV_BLOCK_OFFSET + i]`.
        const u64 doubleSize = static_cast<u64>(BUFFER_SIZE) * 2;
        Memory::GPUSubRegion region = heap.Allocate(jobCtx->GpuCache, doubleSize, 16);
        if (!region.buffer) return;

        memcpy(region.mappedPtr,                          m_CpuScratch,     BUFFER_SIZE);
        memcpy(static_cast<byte*>(region.mappedPtr) + BUFFER_SIZE, m_PrevCpuScratch, BUFFER_SIZE);
        heap.FlushRegion(region);

        // Snapshot current bones into m_PrevCpuScratch for next frame's "previous" half.
        // Done after the upload so this Update's GPU write reflects (frame N current, frame N-1 prev).
        // Next frame's Update will read m_PrevCpuScratch = frame N's bones as its "previous".
        memcpy(m_PrevCpuScratch, m_CpuScratch, BUFFER_SIZE);

        // Write the GAME-frame slot. Render stage K-1 reads slot (K-1)%N while we write slot K%N — distinct in
        // steady state. UAB on the binding (see CreateDescriptors) covers the pipeline-depth race where the GPU
        // falls behind enough that an earlier frame's pending cmd buffer still references this slot.
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

        // Set 4 layout — UAB on the storage-buffer binding. Cycling provides slot isolation in steady-state
        // (game frame K writes slot K%3, render reads (K-1)%3 — distinct slots); UAB is a safety net for
        // cases where the GPU falls behind the CPU pipeline enough that frame K+3's game write hits a slot
        // still referenced by frame K+1's pending cmd buffer (e.g., heavy multi-view frames under per-view
        // 3-submit). VUID 03047 fires without it. Same pattern as GTAOMain's per-render-stage rewrites.
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        // COMPUTE added so the skinning compute pass can read the same SSBO from set 0.
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        const VkDescriptorBindingFlags bindingFlags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bindingFlagsCI.bindingCount  = 1;
        bindingFlagsCI.pBindingFlags = &bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext        = &bindingFlagsCI;
        layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &binding;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_DescriptorSetLayout);

        // Pool sized for MAX_FRAMES_IN_FLIGHT sets; one storage-buffer descriptor each. UAB pool flag pairs with
        // the layout's UPDATE_AFTER_BIND_POOL flag — without both, vkAllocateDescriptorSets fails validation.
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = MAX_FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
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
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            char name[48]; std::snprintf(name, sizeof(name), "BoneMatrix.Slot%u", i);
            VulkanContext::SetDebugName(m_DescriptorSets[i], name);
        }
    }
}
