#include "luthpch.h"
#include "BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"

#include <vma/vk_mem_alloc.h>

namespace Luth
{
    VkBuffer BoneMatrixBuffer::m_Buffer = VK_NULL_HANDLE;
    VmaAllocation BoneMatrixBuffer::m_Allocation = nullptr;
    void* BoneMatrixBuffer::m_MappedData = nullptr;

    VkDescriptorPool BoneMatrixBuffer::m_DescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout BoneMatrixBuffer::m_DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet BoneMatrixBuffer::m_DescriptorSet = VK_NULL_HANDLE;

    std::deque<u32> BoneMatrixBuffer::m_FreeBlocks;
    std::mutex BoneMatrixBuffer::m_Lock;

    void BoneMatrixBuffer::Init()
    {
        CreateBuffer();
        CreateDescriptors();

        // Populate free list
        m_FreeBlocks.clear();
        for (u32 i = 0; i < MAX_SKINNED_ENTITIES; ++i)
            m_FreeBlocks.push_back(i);

        // Fill entire buffer with identity matrices so bind pose "just works"
        Mat4 identity(1.0f);
        u8* dst = static_cast<u8*>(m_MappedData);
        for (u32 i = 0; i < TOTAL_MATRICES; ++i)
            memcpy(dst + i * MATRIX_SIZE, &identity, MATRIX_SIZE);

        LH_CORE_INFO("BoneMatrixBuffer initialized ({0} entities x {1} bones = {2} KB)",
            MAX_SKINNED_ENTITIES, BONES_PER_ENTITY, BUFFER_SIZE / 1024);
    }

    void BoneMatrixBuffer::Shutdown()
    {
        m_FreeBlocks.clear();

        VkDevice device = VulkanContext::Get().GetDevice();

        vkDestroyDescriptorPool(device, m_DescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, m_DescriptorSetLayout, nullptr);

        VulkanAllocator::Unmap(m_Allocation);
        VulkanAllocator::FreeBuffer(m_Buffer, m_Allocation);

        m_Buffer = VK_NULL_HANDLE;
        m_MappedData = nullptr;
    }

    u32 BoneMatrixBuffer::AllocateBlock()
    {
        // Free-list mutation must run on the game stage so the buffer slot
        // a render-stage draw references (via snapshot.boneOffset) is stable.
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "BoneMatrixBuffer::AllocateBlock must run on the game stage");
        std::lock_guard<std::mutex> lock(m_Lock);

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
        std::lock_guard<std::mutex> lock(m_Lock);

        u32 blockIndex = baseIndex / BONES_PER_ENTITY;
        if (blockIndex >= MAX_SKINNED_ENTITIES) return;

        m_FreeBlocks.push_back(blockIndex);
    }

    void BoneMatrixBuffer::UploadBones(u32 baseIndex, const Mat4* matrices, u32 count)
    {
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "BoneMatrixBuffer::UploadBones must run on the game stage");
        if (!m_MappedData) return;
        if (baseIndex + count > TOTAL_MATRICES) return;

        u8* dst = static_cast<u8*>(m_MappedData) + baseIndex * MATRIX_SIZE;
        memcpy(dst, matrices, count * MATRIX_SIZE);
    }

    VkDescriptorSet BoneMatrixBuffer::GetDescriptorSet()
    {
        return m_DescriptorSet;
    }

    VkDescriptorSetLayout BoneMatrixBuffer::GetDescriptorSetLayout()
    {
        return m_DescriptorSetLayout;
    }

    void BoneMatrixBuffer::CreateBuffer()
    {
        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = BUFFER_SIZE;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        m_Allocation = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_CPU_TO_GPU, m_Buffer);
        m_MappedData = VulkanAllocator::Map(m_Allocation);
    }

    void BoneMatrixBuffer::CreateDescriptors()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // 1. Layout
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;

        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_DescriptorSetLayout);

        // 2. Pool
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;

        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_DescriptorPool);

        // 3. Set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_DescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_DescriptorSetLayout;

        vkAllocateDescriptorSets(device, &allocInfo, &m_DescriptorSet);

        // 4. Write
        VkDescriptorBufferInfo bufferInfoDesc{};
        bufferInfoDesc.buffer = m_Buffer;
        bufferInfoDesc.offset = 0;
        bufferInfoDesc.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_DescriptorSet;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufferInfoDesc;

        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}
