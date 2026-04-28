#include "luthpch.h"
#include "MaterialSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
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

        VulkanAllocator::Unmap(m_Allocation);
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
        m_Slots[index].dirty = true;

        return index;
    }

    void MaterialSystem::UnregisterMaterial(u32 index)
    {
        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "MaterialSystem::UnregisterMaterial must run on the game stage");
        std::lock_guard<std::mutex> lock(m_Lock);

        if (index >= MAX_MATERIALS) return;

        m_Slots[index].material = nullptr;
        m_Slots[index].dirty = false;
        m_FreeIndices.push_back(index);
    }

    void MaterialSystem::Update(VkCommandBuffer cmd, u32 gameSlot)
    {
        // Iterate slots and upload dirty ones. Persistently mapped buffer +
        // assumed HOST_COHERENT memory means we can just memcpy. gameSlot is
        // plumbed through but not yet consumed — it lights up once the
        // material SSBO is triple-buffered (sub-task C).
        (void)cmd;
        (void)gameSlot;

        assert(JobSystem::GetCurrentStage() == JobSystem::Stage::Game &&
            "MaterialSystem::Update must run on the game stage");
        std::lock_guard<std::mutex> lock(m_Lock);

        for (u32 i = 0; i < MAX_MATERIALS; ++i)
        {
            if (!m_Slots[i].material) continue;

            // Always refresh GPU data to pick up newly-loaded texture bindless indices
            GPUMaterialData oldData = m_Slots[i].material->GetGPUData();
            m_Slots[i].material->UpdateGPUData();
            const GPUMaterialData& newData = m_Slots[i].material->GetGPUData();

            bool needsUpload = m_Slots[i].dirty
                || m_Slots[i].material->IsGpuDirty()
                || memcmp(&oldData, &newData, MATERIAL_SIZE) != 0;

            if (needsUpload)
            {
                u8* dst = (u8*)m_MappedData + (i * MATERIAL_SIZE);
                memcpy(dst, &newData, MATERIAL_SIZE);

                m_Slots[i].dirty = false;
                m_Slots[i].material->ClearGpuDirty();
            }
        }
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
        bufferInfo.size = MAX_MATERIALS * MATERIAL_SIZE;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        // CPU_TO_GPU for frequent updates via mapping
        m_Allocation = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_CPU_TO_GPU, m_Buffer);
        m_MappedData = VulkanAllocator::Map(m_Allocation);
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
