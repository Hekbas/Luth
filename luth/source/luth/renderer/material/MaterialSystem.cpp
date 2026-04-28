#include "luthpch.h"
#include "MaterialSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/core/FrameData.h"
#include "luth/core/diagnostics/Log.h"
#include "luth/jobs/JobSystem.h"

#include <vma/vk_mem_alloc.h>

namespace Luth
{
    VkBuffer MaterialSystem::m_Buffer = VK_NULL_HANDLE;
    VmaAllocation MaterialSystem::m_Allocation = nullptr;
    void* MaterialSystem::m_MappedData = nullptr;

    VkDescriptorPool MaterialSystem::m_DescriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout MaterialSystem::m_DescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet MaterialSystem::m_DescriptorSet = VK_NULL_HANDLE;

    std::vector<MaterialSystem::MaterialSlot> MaterialSystem::m_Slots;
    std::deque<u32> MaterialSystem::m_FreeIndices;
    std::mutex MaterialSystem::m_Lock;

    void MaterialSystem::Init()
    {
        CreateBuffer();
        CreateDescriptors();

        m_Slots.resize(MAX_MATERIALS);
        for (u32 i = 0; i < MAX_MATERIALS; ++i)
            m_FreeIndices.push_back(i);

        LH_CORE_INFO("Material System Initialized (Max Materials: {0})", MAX_MATERIALS);
    }

    void MaterialSystem::Shutdown()
    {
        // Release shared_ptr references to materials before destroying GPU resources
        m_Slots.clear();
        m_FreeIndices.clear();

        VkDevice device = VulkanContext::Get().GetDevice();

        vkDestroyDescriptorPool(device, m_DescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, m_DescriptorSetLayout, nullptr);

        // Persistent map is owned by VMA (MAPPED_BIT) — vmaDestroyBuffer unmaps.
        VulkanAllocator::FreeBuffer(m_Buffer, m_Allocation);
    }

    u32 MaterialSystem::RegisterMaterial(std::shared_ptr<Material> material)
    {
        // Slot mutation must run on the game stage; concurrent Render(N-1)
        // reads the slot map without locking.
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "MaterialSystem::RegisterMaterial must run on the game stage");
        std::lock_guard<std::mutex> lock(m_Lock);

        if (m_FreeIndices.empty())
        {
            LH_CORE_ERROR("Material System: Out of slots!");
            return 0;
        }

        u32 index = m_FreeIndices.front();
        m_FreeIndices.pop_front();

        m_Slots[index].material = material;
        m_Slots[index].dirtyFramesRemaining = MAX_FRAMES_IN_FLIGHT;

        return index;
    }

    void MaterialSystem::UnregisterMaterial(u32 index)
    {
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "MaterialSystem::UnregisterMaterial must run on the game stage");
        std::lock_guard<std::mutex> lock(m_Lock);

        if (index >= MAX_MATERIALS) return;

        m_Slots[index].material = nullptr;
        m_Slots[index].dirtyFramesRemaining = 0;
        m_FreeIndices.push_back(index);
    }

    void MaterialSystem::Update(VkCommandBuffer cmd, u32 gameSlot)
    {
        // Iterate slots and upload dirty ones to this frame's slice.
        // Persistently mapped buffer + assumed HOST_COHERENT memory means we
        // can just memcpy; non-coherent flush wired in sub-task D.
        (void)cmd;

        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "MaterialSystem::Update must run on the game stage");
        assert(gameSlot < MAX_FRAMES_IN_FLIGHT && "gameSlot out of range");
        std::lock_guard<std::mutex> lock(m_Lock);

        const size_t sliceBaseBytes = static_cast<size_t>(gameSlot) * MAX_MATERIALS * MATERIAL_SIZE;

        for (u32 i = 0; i < MAX_MATERIALS; ++i)
        {
            if (!m_Slots[i].material) continue;

            // Always refresh GPU data to pick up newly-loaded texture bindless indices.
            GPUMaterialData oldData = m_Slots[i].material->GetGPUData();
            m_Slots[i].material->UpdateGPUData();
            const GPUMaterialData& newData = m_Slots[i].material->GetGPUData();

            // A fresh change re-arms the countdown so the new data propagates to
            // all MAX_FRAMES_IN_FLIGHT slices over consecutive iterations. Clear
            // IsGpuDirty here (not after the last write) so a sticky flag doesn't
            // re-arm the countdown forever.
            const bool changed = m_Slots[i].material->IsGpuDirty()
                || memcmp(&oldData, &newData, MATERIAL_SIZE) != 0;

            if (changed)
            {
                m_Slots[i].dirtyFramesRemaining = MAX_FRAMES_IN_FLIGHT;
                m_Slots[i].material->ClearGpuDirty();
            }

            if (m_Slots[i].dirtyFramesRemaining > 0)
            {
                u8* dst = (u8*)m_MappedData + sliceBaseBytes + (i * MATERIAL_SIZE);
                memcpy(dst, &newData, MATERIAL_SIZE);
                m_Slots[i].dirtyFramesRemaining--;
            }
        }

        // Flush this frame's slice. No-op on HOST_COHERENT memory; required when
        // the chosen memory type lacks coherence (e.g. discrete GPU + ReBAR).
        const VkDeviceSize sliceBytes = static_cast<VkDeviceSize>(MAX_MATERIALS) * MATERIAL_SIZE;
        VulkanAllocator::FlushSlice(m_Allocation, sliceBaseBytes, sliceBytes);
    }

    VkDescriptorSet MaterialSystem::GetDescriptorSet()
    {
        return m_DescriptorSet;
    }

    VkDescriptorSetLayout MaterialSystem::GetDescriptorSetLayout()
    {
        return m_DescriptorSetLayout;
    }

    void MaterialSystem::CreateBuffer()
    {
        VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        // MAX_FRAMES_IN_FLIGHT slices — frame N writes its slice without aliasing
        // GPU N-1 / N-2 reads. obj.materialIndex (baked in BuildGPUObjectBuffer)
        // already encodes the slice base, so the descriptor stays VK_WHOLE_SIZE.
        bufferInfo.size  = static_cast<VkDeviceSize>(MAX_MATERIALS) * MATERIAL_SIZE * MAX_FRAMES_IN_FLIGHT;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        m_Allocation = VulkanAllocator::AllocateMappedSequentialBuffer(bufferInfo, m_Buffer, &m_MappedData);
    }

    void MaterialSystem::CreateDescriptors()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // 1. Layout
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT; // Accessible in frag/compute

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
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_Buffer;
        bufferInfo.offset = 0;
        bufferInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_DescriptorSet;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}
