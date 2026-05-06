#include "luthpch.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanPipeline.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
#include "luth/renderer/resources/Buffer.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/draw/DrawCommand.h"

namespace Luth
{
    namespace
    {
        // Full PBR vertex: position + normal + uv0 + uv1 + tangent (52 bytes)
        BufferLayout MakePBRVertexLayout()
        {
            return BufferLayout{
                { ShaderDataType::Float3, "a_Position"  },
                { ShaderDataType::Float3, "a_Normal"    },
                { ShaderDataType::Float2, "a_TexCoord0" },
                { ShaderDataType::Float2, "a_TexCoord1" },
                { ShaderDataType::Float3, "a_Tangent"   }
            };
        }

        // Skinned vertex: PBR layout + bone IDs (int4) + bone weights (float4) — 84 bytes total
        BufferLayout MakeSkinnedVertexLayout()
        {
            return BufferLayout{
                { ShaderDataType::Float3, "a_Position"    },
                { ShaderDataType::Float3, "a_Normal"      },
                { ShaderDataType::Float2, "a_TexCoord0"   },
                { ShaderDataType::Float2, "a_TexCoord1"   },
                { ShaderDataType::Float3, "a_Tangent"     },
                { ShaderDataType::Int4,   "a_BoneIDs"     },
                { ShaderDataType::Float4, "a_BoneWeights" }
            };
        }

        // Position-only attribute but the vertex buffer still uses the full PBR stride —
        // override stride so buffer reads align. Used by shadow / depth-prepass / selection
        // pipelines that only consume a_Position from the same VB as PBR draws.
        std::pair<std::vector<VkVertexInputBindingDescription>, std::vector<VkVertexInputAttributeDescription>>
        MakePositionOnlyWithFullStride()
        {
            BufferLayout layout = { { ShaderDataType::Float3, "a_Position" } };
            auto bindings = layout.GetBindingDescriptions();
            auto attribs  = layout.GetAttributeDescriptions();
            if (!bindings.empty())
                bindings[0].stride = sizeof(float) * (3 + 3 + 2 + 2 + 3); // full PBR stride (52 bytes)
            return { std::move(bindings), std::move(attribs) };
        }
    }

    void RenderPipeline::CreatePipelines()
    {
        // Ensure Set 5 descriptor layout exists (idempotent — safe to call on hot-reload)
        InitObjectSSBODescriptorLayout();

        BuildPBRPipelines();
        BuildDepthPrepassPipelines();
        BuildSelectionPipelines();
        BuildPostPipelines();
        BuildOutlinePipeline();
        BuildGridPipeline();
        // Shadow + skybox pipelines are owned by LightingSubsystem; the orchestrator
        // calls m_Lighting.BuildPipelines after this returns (Set 5 layout now ready).
    }

    // =========================================================================
    //  PBR — rigid + skinned pipeline managers (lazy, keyed by shader+renderMode)
    // =========================================================================
    void RenderPipeline::BuildPBRPipelines()
    {
        // 6-set layout for geometry pipelines (Set 5 = GPUObjectData SSBO, no push constants)
        std::vector<VkDescriptorSetLayout> geoLayouts = {
            m_Global.GetSetLayout(),                                    // Set 0
            VulkanContext::Get().GetBindlessSet().GetLayout(),   // Set 1
            MaterialSystem::GetDescriptorSetLayout(),            // Set 2
            m_Lighting.GetSetLayout(),                                    // Set 3
            BoneMatrixBuffer::GetDescriptorSetLayout(),          // Set 4
            m_ObjectSSBODescLayout                               // Set 5
        };

        // ---- Rigid PBR ----
        {
            auto pbrLayout = MakePBRVertexLayout();
            auto bindingDescs = pbrLayout.GetBindingDescriptions();
            auto attribDescs  = pbrLayout.GetAttributeDescriptions();

            m_GeoPipelineManager.Init(geoLayouts,
                [bindingDescs, attribDescs](Material::RenderMode mode, Material::CullMode cullMode, VkPolygonMode polygonMode) -> PipelineConfig
                {
                    PipelineConfig config;
                    config.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_UINT };
                    config.depthFormat = VK_FORMAT_D32_SFLOAT;
                    config.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                    config.bindingDescriptions = bindingDescs;
                    config.attributeDescriptions = attribDescs;
                    // No push constants — per-object data comes from GPUObjectData SSBO (Set 5)
                    config.polygonMode = polygonMode;
                    // LESS_OR_EQUAL so opaques pass the DepthPrepass values (written with LESS)
                    // exactly, while cutouts/transparents still Z-test against the prepass depth.
                    config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

                    switch (mode)
                    {
                        case Material::RenderMode::Opaque:
                            config.depthTest = true; config.depthWrite = true;
                            config.blendEnabled = false;
                            break;
                        case Material::RenderMode::Cutout:
                            config.depthTest = true; config.depthWrite = true;
                            config.blendEnabled = false;
                            break;
                        case Material::RenderMode::Transparent:
                        case Material::RenderMode::Fade:
                            config.depthTest = true; config.depthWrite = false;
                            config.blendEnabled = true;
                            break;
                    }

                    // Apply material cull mode
                    switch (cullMode)
                    {
                        case Material::CullMode::Back:  config.cullMode = VK_CULL_MODE_BACK_BIT;  break;
                        case Material::CullMode::Front: config.cullMode = VK_CULL_MODE_FRONT_BIT; break;
                        case Material::CullMode::None:  config.cullMode = VK_CULL_MODE_NONE;      break;
                    }

                    return config;
                });
        }

        // ---- Skinned PBR ----
        {
            auto skinnedLayout = MakeSkinnedVertexLayout();
            auto skinnedBindingDescs = skinnedLayout.GetBindingDescriptions();
            auto skinnedAttribDescs  = skinnedLayout.GetAttributeDescriptions();

            m_GeoSkinnedPipelineManager.Init(geoLayouts,
                [skinnedBindingDescs, skinnedAttribDescs](Material::RenderMode mode, Material::CullMode cullMode, VkPolygonMode polygonMode) -> PipelineConfig
                {
                    PipelineConfig config;
                    config.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_UINT };
                    config.depthFormat = VK_FORMAT_D32_SFLOAT;
                    config.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                    config.bindingDescriptions = skinnedBindingDescs;
                    config.attributeDescriptions = skinnedAttribDescs;
                    // No push constants — per-object data comes from GPUObjectData SSBO (Set 5)
                    config.polygonMode = polygonMode;
                    config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL; // matches DepthPrepass output

                    switch (mode)
                    {
                        case Material::RenderMode::Opaque:
                            config.depthTest = true; config.depthWrite = true;
                            config.blendEnabled = false;
                            break;
                        case Material::RenderMode::Cutout:
                            config.depthTest = true; config.depthWrite = true;
                            config.blendEnabled = false;
                            break;
                        case Material::RenderMode::Transparent:
                        case Material::RenderMode::Fade:
                            config.depthTest = true; config.depthWrite = false;
                            config.blendEnabled = true;
                            break;
                    }

                    switch (cullMode)
                    {
                        case Material::CullMode::Back:  config.cullMode = VK_CULL_MODE_BACK_BIT;  break;
                        case Material::CullMode::Front: config.cullMode = VK_CULL_MODE_FRONT_BIT; break;
                        case Material::CullMode::None:  config.cullMode = VK_CULL_MODE_NONE;      break;
                    }

                    return config;
                });
        }
    }

    // =========================================================================
    //  Depth prepass — rigid + skinned camera-space depth-only
    // =========================================================================
    void RenderPipeline::BuildDepthPrepassPipelines()
    {
        std::vector<VkDescriptorSetLayout> geoLayouts = {
            m_Global.GetSetLayout(),
            VulkanContext::Get().GetBindlessSet().GetLayout(),
            MaterialSystem::GetDescriptorSetLayout(),
            m_Lighting.GetSetLayout(),
            BoneMatrixBuffer::GetDescriptorSetLayout(),
            m_ObjectSSBODescLayout
        };

        auto [shadowBindingDescs, shadowAttribDescs] = MakePositionOnlyWithFullStride();

        // Reuses the shadow frag SPIR-V (empty `void main(){}`) as the null fragment.
        // Rigid variant uses the position-only binding/attribs (full PBR stride).
        if (!m_DepthPrepassVertSpv.empty() && !m_Lighting.GetShadowFragSpv().empty())
        {
            PipelineConfig depthPrepassConfig;
            depthPrepassConfig.colorFormats = {}; // depth-only
            depthPrepassConfig.depthFormat  = VK_FORMAT_D32_SFLOAT;
            depthPrepassConfig.depthTest    = true;
            depthPrepassConfig.depthWrite   = true;
            depthPrepassConfig.depthCompareOp = VK_COMPARE_OP_LESS;
            depthPrepassConfig.blendEnabled = false;
            depthPrepassConfig.cullMode     = VK_CULL_MODE_BACK_BIT;
            depthPrepassConfig.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            depthPrepassConfig.bindingDescriptions   = shadowBindingDescs; // position-only + full-vertex stride
            depthPrepassConfig.attributeDescriptions = shadowAttribDescs;

            m_DepthPrepassPipeline = std::make_unique<VKPipeline>(
                depthPrepassConfig, m_DepthPrepassVertSpv, m_Lighting.GetShadowFragSpv(), geoLayouts);
        }

        if (!m_DepthPrepassSkinnedVertSpv.empty() && !m_Lighting.GetShadowFragSpv().empty())
        {
            auto skinnedLayout = MakeSkinnedVertexLayout();
            auto skinnedBindingDescs = skinnedLayout.GetBindingDescriptions();
            auto skinnedAttribDescs  = skinnedLayout.GetAttributeDescriptions();

            PipelineConfig depthPrepassSkinnedConfig;
            depthPrepassSkinnedConfig.colorFormats = {};
            depthPrepassSkinnedConfig.depthFormat  = VK_FORMAT_D32_SFLOAT;
            depthPrepassSkinnedConfig.depthTest    = true;
            depthPrepassSkinnedConfig.depthWrite   = true;
            depthPrepassSkinnedConfig.depthCompareOp = VK_COMPARE_OP_LESS;
            depthPrepassSkinnedConfig.blendEnabled = false;
            depthPrepassSkinnedConfig.cullMode     = VK_CULL_MODE_BACK_BIT;
            depthPrepassSkinnedConfig.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            depthPrepassSkinnedConfig.bindingDescriptions   = skinnedBindingDescs;
            depthPrepassSkinnedConfig.attributeDescriptions = skinnedAttribDescs;

            m_DepthPrepassSkinnedPipeline = std::make_unique<VKPipeline>(
                depthPrepassSkinnedConfig, m_DepthPrepassSkinnedVertSpv, m_Lighting.GetShadowFragSpv(), geoLayouts);
        }
    }

    // =========================================================================
    //  Selection mask — rigid + skinned
    // =========================================================================
    void RenderPipeline::BuildSelectionPipelines()
    {
        // 5-set layout (Set 0-4) + per-draw ObjectPushConstants
        std::vector<VkDescriptorSetLayout> layouts = {
            m_Global.GetSetLayout(),
            VulkanContext::Get().GetBindlessSet().GetLayout(),
            MaterialSystem::GetDescriptorSetLayout(),
            m_Lighting.GetSetLayout(),
            BoneMatrixBuffer::GetDescriptorSetLayout()
        };

        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(ObjectPushConstants);

        auto [shadowBindingDescs, shadowAttribDescs] = MakePositionOnlyWithFullStride();

        // ---- Static variant ----
        if (!m_SelectionMaskVertSpv.empty() && !m_SelectionMaskFragSpv.empty())
        {
            PipelineConfig maskConfig;
            maskConfig.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
            maskConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
            maskConfig.depthTest = true; maskConfig.depthWrite = true;
            maskConfig.blendEnabled = false;
            maskConfig.cullMode = VK_CULL_MODE_BACK_BIT;
            maskConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            maskConfig.bindingDescriptions = shadowBindingDescs;
            maskConfig.attributeDescriptions = shadowAttribDescs;
            maskConfig.pushConstantRanges = { pushConstantRange };

            m_SelectionMaskPipeline = std::make_unique<VKPipeline>(
                maskConfig, m_SelectionMaskVertSpv, m_SelectionMaskFragSpv, layouts);
        }

        // ---- Skinned variant ----
        if (!m_SelectionMaskSkinnedVertSpv.empty() && !m_SelectionMaskFragSpv.empty())
        {
            auto skinnedLayout = MakeSkinnedVertexLayout();
            auto skinnedBindingDescs = skinnedLayout.GetBindingDescriptions();
            auto skinnedAttribDescs  = skinnedLayout.GetAttributeDescriptions();

            PipelineConfig maskSkinnedConfig;
            maskSkinnedConfig.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
            maskSkinnedConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
            maskSkinnedConfig.depthTest = true; maskSkinnedConfig.depthWrite = true;
            maskSkinnedConfig.blendEnabled = false;
            maskSkinnedConfig.cullMode = VK_CULL_MODE_BACK_BIT;
            maskSkinnedConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            maskSkinnedConfig.bindingDescriptions = skinnedBindingDescs;
            maskSkinnedConfig.attributeDescriptions = skinnedAttribDescs;
            maskSkinnedConfig.pushConstantRanges = { pushConstantRange };

            m_SelectionMaskSkinnedPipeline = std::make_unique<VKPipeline>(
                maskSkinnedConfig, m_SelectionMaskSkinnedVertSpv, m_SelectionMaskFragSpv, layouts);
        }
    }

    // =========================================================================
    //  Post-process — bloom extract, bloom blur, composite
    // =========================================================================
    void RenderPipeline::BuildPostPipelines()
    {
        if (m_FullscreenVertSpv.empty() || m_PPDescSetLayout == VK_NULL_HANDLE) return;

        std::vector<VkDescriptorSetLayout> ppLayouts = { m_PPDescSetLayout };

        // Bloom extract: push constant = float threshold + pad
        if (!m_BloomExtractFragSpv.empty())
        {
            VkPushConstantRange bloomExtractPC{};
            bloomExtractPC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bloomExtractPC.offset = 0;
            bloomExtractPC.size = sizeof(float) * 4; // threshold + pad[3]

            PipelineConfig bloomExtractConfig;
            bloomExtractConfig.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };
            bloomExtractConfig.depthFormat = VK_FORMAT_UNDEFINED;
            bloomExtractConfig.depthTest = false; bloomExtractConfig.depthWrite = false;
            bloomExtractConfig.blendEnabled = false;
            bloomExtractConfig.cullMode = VK_CULL_MODE_NONE;
            bloomExtractConfig.pushConstantRanges = { bloomExtractPC };
            m_BloomExtractPipeline = std::make_unique<VKPipeline>(
                bloomExtractConfig, m_FullscreenVertSpv, m_BloomExtractFragSpv, ppLayouts);
        }

        // Bloom blur: push constant = vec2 direction + pad
        if (!m_BloomBlurFragSpv.empty())
        {
            VkPushConstantRange bloomBlurPC{};
            bloomBlurPC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bloomBlurPC.offset = 0;
            bloomBlurPC.size = sizeof(float) * 4; // direction.xy + pad[2]

            PipelineConfig bloomBlurConfig;
            bloomBlurConfig.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };
            bloomBlurConfig.depthFormat = VK_FORMAT_UNDEFINED;
            bloomBlurConfig.depthTest = false; bloomBlurConfig.depthWrite = false;
            bloomBlurConfig.blendEnabled = false;
            bloomBlurConfig.cullMode = VK_CULL_MODE_NONE;
            bloomBlurConfig.pushConstantRanges = { bloomBlurPC };
            m_BloomBlurPipeline = std::make_unique<VKPipeline>(
                bloomBlurConfig, m_FullscreenVertSpv, m_BloomBlurFragSpv, ppLayouts);
        }

        // PostProcess composite: no push constants, UBO at binding 2
        if (!m_PostProcessFragSpv.empty())
        {
            PipelineConfig ppConfig;
            ppConfig.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM }; // LDR output
            ppConfig.depthFormat = VK_FORMAT_UNDEFINED;
            ppConfig.depthTest = false; ppConfig.depthWrite = false;
            ppConfig.blendEnabled = false;
            ppConfig.cullMode = VK_CULL_MODE_NONE;
            m_PostProcessPipeline = std::make_unique<VKPipeline>(
                ppConfig, m_FullscreenVertSpv, m_PostProcessFragSpv, ppLayouts);
        }
    }

    // =========================================================================
    //  Outline — alpha-blended fullscreen pass
    // =========================================================================
    void RenderPipeline::BuildOutlinePipeline()
    {
        if (m_FullscreenVertSpv.empty() || m_OutlineFragSpv.empty() || m_OutlineDescSetLayout == VK_NULL_HANDLE) return;

        std::vector<VkDescriptorSetLayout> outlineLayouts = { m_OutlineDescSetLayout };

        VkPushConstantRange outlinePC{};
        outlinePC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        outlinePC.offset = 0;
        outlinePC.size = sizeof(float) * 8; // outlineWidth + texelSize(vec2) + outlineColor(vec4) + occludedAlpha = 32 bytes

        PipelineConfig outlineConfig;
        outlineConfig.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM }; // writes to LDR output with blending
        outlineConfig.depthFormat = VK_FORMAT_UNDEFINED;
        outlineConfig.depthTest = false; outlineConfig.depthWrite = false;
        outlineConfig.blendEnabled = true; // alpha blend the outline on top
        outlineConfig.cullMode = VK_CULL_MODE_NONE;
        outlineConfig.pushConstantRanges = { outlinePC };
        m_OutlinePipeline = std::make_unique<VKPipeline>(
            outlineConfig, m_FullscreenVertSpv, m_OutlineFragSpv, outlineLayouts);
    }

    // =========================================================================
    //  Grid — editor-only infinite grid fullscreen pass
    // =========================================================================
    void RenderPipeline::BuildGridPipeline()
    {
        if (m_FullscreenVertSpv.empty() || m_GridFragSpv.empty() || m_GridDescSetLayout == VK_NULL_HANDLE) return;

        std::vector<VkDescriptorSetLayout> gridLayouts = { m_GridDescSetLayout };

        VkPushConstantRange gridPC{};
        gridPC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        gridPC.offset = 0;
        gridPC.size = sizeof(float) * 16; // 3 vec4 colors + 4 floats = 16 floats = 64 bytes

        PipelineConfig gridConfig;
        gridConfig.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT }; // HDR scene color
        gridConfig.depthFormat = VK_FORMAT_UNDEFINED;
        gridConfig.depthTest = false; gridConfig.depthWrite = false;
        gridConfig.blendEnabled = true; // alpha-composite grid over scene
        gridConfig.cullMode = VK_CULL_MODE_NONE;
        gridConfig.pushConstantRanges = { gridPC };
        m_GridPipeline = std::make_unique<VKPipeline>(
            gridConfig, m_FullscreenVertSpv, m_GridFragSpv, gridLayouts);
    }
}
