#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/backend/vulkan/VulkanComputePipeline.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/resources/Buffer.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/resources/AssetManager.h"

#include <vma/vk_mem_alloc.h>

namespace Luth
{

    void RenderPipeline::InitObjectSSBODescriptorLayout()
    {
        if (m_ObjectSSBODescLayout != VK_NULL_HANDLE)
            return;

        VkDevice device = VulkanContext::Get().GetDevice();

        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &binding;

        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_ObjectSSBODescLayout);
    }

    void RenderPipeline::InitGPUObjectBuffers()
    {
        auto allocBuffer = [](u64 size, VkBufferUsageFlags usage,
                               VkBuffer& buf, VmaAllocation& alloc, void*& mapped)
        {
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size  = size;
            info.usage = usage;
            alloc  = VulkanAllocator::AllocateBuffer(info, VMA_MEMORY_USAGE_CPU_TO_GPU, buf);
            mapped = VulkanAllocator::Map(alloc);
        };

        allocBuffer(
            RenderPipeline::k_MaxGPUObjects * sizeof(GPUObjectData),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            m_ObjectSSBO, m_ObjectSSBOAlloc, m_ObjectSSBOMapped);

        // Indirect buffer holds 5 regions (camera + 4 cascades), each with RenderPipeline::k_IndirectRegionStride commands.
        allocBuffer(
            RenderPipeline::k_IndirectRegionCount * RenderPipeline::k_IndirectRegionStride * sizeof(VkDrawIndexedIndirectCommand),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            m_IndirectBuffer, m_IndirectBufferAlloc, m_IndirectBufferMapped);

        // Create Set 5 descriptor pool + set for the ObjectSSBO (graphics pipeline)
        VkDevice device = VulkanContext::Get().GetDevice();

        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets       = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_ObjectSSBODescPool);

        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool     = m_ObjectSSBODescPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_ObjectSSBODescLayout;
        vkAllocateDescriptorSets(device, &allocInfo, &m_ObjectSSBODescSet);

        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_ObjectSSBO;
        bufInfo.offset = 0;
        bufInfo.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet          = m_ObjectSSBODescSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo     = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    void RenderPipeline::InitCullPipeline()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Descriptor layout: binding 0 = ObjectSSBO (read), binding 1 = IndirectBuffer (write)
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_CullDescLayout);

        VulkanContext::Get().GetDescriptorAllocator().Allocate(m_CullDescLayout, m_CullDescSet);

        VkDescriptorBufferInfo objInfo{};
        objInfo.buffer = m_ObjectSSBO;
        objInfo.offset = 0;
        objInfo.range  = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo indInfo{};
        indInfo.buffer = m_IndirectBuffer;
        indInfo.offset = 0;
        indInfo.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_CullDescSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo     = &objInfo;
        writes[1]                 = writes[0];
        writes[1].dstBinding      = 1;
        writes[1].pBufferInfo     = &indInfo;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        // Push constant range: 6 frustum planes (96B) + objectCount (4B) + destOffset (4B) = 104B
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = sizeof(Vec4) * 6 + sizeof(u32) * 2;

        auto cullShader = ShaderLibrary::LoadEngine("shaders/gpu_cull.comp");
        auto spv = cullShader ? cullShader->GetSpirV() : std::vector<u32>{};
        if (spv.empty())
        {
            LH_CORE_ERROR("RenderingSystem: Failed to load gpu_cull.comp!");
            return;
        }

        m_CullPipeline = std::make_unique<VKComputePipeline>(
            spv,
            std::vector<VkDescriptorSetLayout>{ m_CullDescLayout },
            std::vector<VkPushConstantRange>{ pcRange });
    }

    u32 RenderPipeline::EnsureMaterialRegistered(std::shared_ptr<Material> material)
    {
        auto it = m_MaterialSlotMap.find(material->Handle);
        if (it != m_MaterialSlotMap.end())
            return it->second;

        u32 slot = MaterialSystem::RegisterMaterial(material);
        m_MaterialSlotMap[material->Handle] = slot;
        return slot;
    }

    void RenderPipeline::BuildGPUObjectBuffer(const RenderSnapshot& snapshot)
    {
        auto* objectData   = static_cast<GPUObjectData*>(m_ObjectSSBOMapped);
        auto* indirectCmds = static_cast<VkDrawIndexedIndirectCommand*>(m_IndirectBufferMapped);
        u32   count        = 0;

        // Rebuild entity lookup table here (consumed by GeometryPass + mouse picking)
        // index 0 = null sentinel; valid entities start at index 1
        m_EntityLookup.clear();
        m_EntityLookup.push_back(entt::null);
        m_EntityToSSBOIndex.clear();

        for (const MeshDrawSnapshot& meshSnap : snapshot.meshes)
        {
            if (count >= RenderPipeline::k_MaxGPUObjects) break;

            // Snapshot already filtered out invalid model + OOB mesh index, but
            // GetMesh + GetIndexBuffer still need the model lookup for AABB and IB.
            auto model = AssetManager::GetAsset<Model>(meshSnap.modelUUID);
            if (!model) continue;
            const auto& meshesData = model->GetMeshesData();
            auto mesh = model->GetMesh(meshSnap.meshIndex);
            if (!mesh) continue;

            GPUObjectData& obj = objectData[count];
            obj.model = meshSnap.worldMatrix;

            // Bounding sphere from BindPoseAABB (local space)
            const auto& aabb   = meshesData[meshSnap.meshIndex].BindPoseAABB;
            obj.boundingSphere = Vec4(aabb.Center(), Math::Length(aabb.Extents()));

            // Material slot
            u32 matSlot = 0;
            if (meshSnap.materialUUID.IsValid()) {
                auto it = m_MaterialSlotMap.find(meshSnap.materialUUID);
                if (it != m_MaterialSlotMap.end()) matSlot = it->second;
            }
            obj.materialIndex = matSlot;
            obj.shadeMode     = static_cast<u32>(m_System.m_ShadeMode);
            // entityID is 1-indexed so the fragment shader output matches m_EntityLookup
            obj.entityID      = (u32)m_EntityLookup.size();  // assigned before push_back
            obj.boneOffset    = meshSnap.boneOffset;          // baked in CaptureSnapshot

            entt::entity entity = static_cast<entt::entity>(meshSnap.entity);
            m_EntityLookup.push_back(entity);      // m_EntityLookup[count + 1] = entity
            m_EntityToSSBOIndex[entity] = count;   // entity → 0-based SSBO index

            auto* ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer()).get();
            obj.indexCount   = ib ? ib->GetCount() : 0;
            obj.firstIndex   = 0;
            obj.vertexOffset = 0;
            obj._pad         = 0;

            // Indirect command — instanceCount=1; per-region GPU cull zeros it if culled.
            // firstInstance = SSBO index (gl_BaseInstance in shader → objects[gl_BaseInstance]).
            // Duplicate into all RenderPipeline::k_IndirectRegionCount regions (camera + 4 cascades) so each
            // region has its own independently-cullable command for this object.
            VkDrawIndexedIndirectCommand baseCmd{};
            baseCmd.indexCount    = obj.indexCount;
            baseCmd.instanceCount = 1;
            baseCmd.firstIndex    = 0;
            baseCmd.vertexOffset  = 0;
            baseCmd.firstInstance = count;
            for (u32 r = 0; r < RenderPipeline::k_IndirectRegionCount; ++r)
                indirectCmds[r * RenderPipeline::k_IndirectRegionStride + count] = baseCmd;
            count++;
        }

        m_GPUObjectCount = count;
    }

    void RenderPipeline::UploadLightUBO(const LightUniforms& lights)
    {
        m_LightUniformBuffer->SetData(&lights, sizeof(LightUniforms));
    }
}
