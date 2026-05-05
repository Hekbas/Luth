#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/core/FrameData.h"
#include "luth/core/time/Time.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"

namespace Luth
{
    // Creates the shared Set 0 layout. The per-view UBO + descriptor set
    // are allocated lazily by EnsureViewResources; see ViewResources.cpp.
    void RenderPipeline::InitGlobalUniforms()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Set 0 layout: 0 = GlobalUBO, 1-3 = IBL samplers, 4 = GTAO sampler, 5 = GTAO UBO
        VkDescriptorSetLayoutBinding bindings[6] = {};

        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[4].binding = 4;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[5].binding = 5;
        bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[5].descriptorCount = 1;
        bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        // Bindings 0 (Global UBO) + 5 (GTAO UBO) are rebound per render-stage to fresh
        // GPUTaggedPageAllocator regions. Samplers (1-4) are stable.
        VkDescriptorBindingFlags bindingFlags[6] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0, 0, 0, 0,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        bindingFlagsInfo.bindingCount  = 6;
        bindingFlagsInfo.pBindingFlags = bindingFlags;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.pNext = &bindingFlagsInfo;
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layoutInfo.bindingCount = 6;
        layoutInfo.pBindings = bindings;

        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_GlobalSetLayout);
    }

    // Allocates a per-frame UBO region from GPUTaggedPageAllocator and rebinds
    // Set 0 binding 0 + Grid set binding 0 to it. Replaces the prior in-place
    // memcpy into a single persistent VKUniformBuffer that raced GPU N-2 reads.
    void RenderPipeline::UpdateGlobalUniforms(const CameraParams& camera, const CascadeData& cascades, const DirectionalLightShadowParams& shadowParams)
    {
        // Cache for downstream per-frame reads (Execute + capturedFrame snapshot).
        m_FrameCascades     = cascades;
        m_FrameShadowParams = shadowParams;

        GlobalUniforms ubo{};
        ubo.view = camera.view;
        ubo.projection = camera.projection;
        ubo.projection[1][1] *= -1.0f;  // Vulkan Y-flip (shader only, not ImGuizmo)
        ubo.viewProjection = ubo.projection * ubo.view;
        ubo.cameraPos = camera.position;
        ubo.time = Time::GetTime();
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            ubo.lightSpaceMatrix[i] = cascades.lightSpaceMatrix[i];
        ubo.cascadeSplitsViewZ = cascades.splitsViewZ;
        // Negative bias (sentinel) disables shadows entirely in the PBR shader.
        ubo.shadowBias       = shadowParams.castShadows ? shadowParams.shadowBias : Vec4(-1.0f);
        ubo.shadowNormalBias = shadowParams.shadowNormalBias;
        ubo.cascadeTexelSize = cascades.texelSize;
        ubo.iblIntensity    = camera.iblIntensity;
        ubo.skyboxIntensity = camera.skyboxIntensity;
        ubo.debugVisualizeCascades = shadowParams.debugVisualizeCascades ? 1.0f : 0.0f;
        ubo.cascadeBlendWidth      = shadowParams.cascadeBlendWidth;

        m_CachedViewProj = ubo.viewProjection;

        if (!m_CurrentViewResources || m_CurrentViewResources->globalDescriptorSet == VK_NULL_HANDLE) return;

        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return;
        jobCtx->GpuCache.CurrentTag = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());

        auto& heap   = Memory::GPUTaggedPageAllocator::Get();
        const u64 al = VulkanContext::Get().GetMinUniformBufferAlignment();
        Memory::GPUSubRegion region = heap.Allocate(jobCtx->GpuCache, sizeof(GlobalUniforms), al);
        if (!region.buffer) return;

        memcpy(region.mappedPtr, &ubo, sizeof(GlobalUniforms));
        heap.FlushRegion(region);

        VkDescriptorBufferInfo bi{};
        bi.buffer = region.buffer;
        bi.offset = region.offset;
        bi.range  = region.size;

        // Set 0 binding 0 + Grid set binding 0 share the same per-frame region.
        VkWriteDescriptorSet writes[2] = {};
        writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[0].dstSet          = m_CurrentViewResources->globalDescriptorSet;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &bi;

        u32 n = 1;
        if (m_CurrentViewResources->gridDescSet != VK_NULL_HANDLE)
        {
            writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[1].dstSet          = m_CurrentViewResources->gridDescSet;
            writes[1].dstBinding      = 0;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[1].descriptorCount = 1;
            writes[1].pBufferInfo     = &bi;
            ++n;
        }

        vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), n, writes, 0, nullptr);
    }
}
