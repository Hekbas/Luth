#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/core/time/Time.h"

namespace Luth
{
    // InitGlobalUniforms creates the Set 0 descriptor-set layout shared by
    // every view. The per-view GlobalUBO buffer + descriptor set are
    // allocated lazily by EnsureViewResources() on first Execute for each
    // FrameTargets (see ViewResources.cpp). IBL bindings 1-3 are written
    // into each view's set by WriteViewGlobalSet once the IBL textures are
    // ready — InitIBLResources populates m_IrradianceMap / m_PrefilteredMap
    // / m_BRDFLut and ReloadSkybox rebuilds every cached view's set.
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

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 6;
        layoutInfo.pBindings = bindings;

        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_GlobalSetLayout);
    }

    // UpdateGlobalUniforms writes the per-view GlobalUBO content. Called by
    // RS::RenderToView before Execute. The ViewResources for the active view
    // is cached on the pipeline (m_CurrentViewResources) by PrepareForTargets
    // — this function only writes buffer contents, never touches descriptor
    // state, so it's safe to run inside Execute ordering without aliasing
    // other views' GPU work.
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

        if (m_CurrentViewResources && m_CurrentViewResources->globalUniformBuffer)
            m_CurrentViewResources->globalUniformBuffer->SetData(&ubo, sizeof(GlobalUniforms));
        m_CachedViewProj = ubo.viewProjection;
    }
}
