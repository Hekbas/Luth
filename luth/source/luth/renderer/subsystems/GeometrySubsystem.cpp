#include "luthpch.h"
#include "luth/renderer/subsystems/GeometrySubsystem.h"
#include "luth/renderer/subsystems/LightingSubsystem.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/FrameDebugger.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/material/MaterialSystem.h"
#include "luth/renderer/resources/BoneMatrixBuffer.h"
#include "luth/renderer/resources/Buffer.h"
#include "luth/renderer/resources/Model.h"
#include "luth/renderer/draw/DrawCommand.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/core/FrameData.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/jobs/JobSystem.h"
#include "luth/memory/GPUTaggedPageAllocator.h"
#include "luth/resources/AssetManager.h"

namespace Luth
{
    namespace {
        BufferLayout MakePBRVertexLayout() {
            return BufferLayout{
                { ShaderDataType::Float3, "a_Position"  },
                { ShaderDataType::Float3, "a_Normal"    },
                { ShaderDataType::Float2, "a_TexCoord0" },
                { ShaderDataType::Float2, "a_TexCoord1" },
                { ShaderDataType::Float3, "a_Tangent"   }
            };
        }
        BufferLayout MakeSkinnedVertexLayout() {
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
        // Position-only attribute with full PBR vertex stride — depth-prepass and shadow
        // pipelines reuse the PBR vertex buffer but only consume a_Position.
        std::pair<std::vector<VkVertexInputBindingDescription>, std::vector<VkVertexInputAttributeDescription>>
        MakePositionOnlyWithFullStride() {
            BufferLayout layout = { { ShaderDataType::Float3, "a_Position" } };
            auto bindings = layout.GetBindingDescriptions();
            auto attribs  = layout.GetAttributeDescriptions();
            // Stride mirrors MakePBRVertexLayout: Position3 + Normal3 + TexCoord0_2 + TexCoord1_2 + Tangent3.
            if (!bindings.empty()) bindings[0].stride = sizeof(float) * (3 + 3 + 2 + 2 + 3);
            return { std::move(bindings), std::move(attribs) };
        }
    }

    void GeometrySubsystem::Init(RenderPipeline& pipeline)
    {
        m_Pipeline = &pipeline;

        auto loadSpv = [](const char* relPath) -> std::vector<u32> {
            auto sh = ShaderLibrary::LoadEngine(relPath);
            return sh ? sh->GetSpirV() : std::vector<u32>{};
        };
        m_PBRVertSpv                 = loadSpv("shaders/pbr.vert");
        m_PBRFragSpv                 = loadSpv("shaders/pbr.slang");
        m_PBRSkinnedVertSpv          = loadSpv("shaders/pbr_skinned.vert");
        m_DepthPrepassVertSpv        = loadSpv("shaders/depthPrepass.vert");
        m_DepthPrepassSkinnedVertSpv = loadSpv("shaders/depthPrepass_skinned.vert");
        m_SlimGBufferVertSpv         = loadSpv("shaders/slim_gbuffer.vert");
        m_SlimGBufferSkinnedVertSpv  = loadSpv("shaders/slim_gbuffer_skinned.vert");
        m_SlimGBufferFragSpv         = loadSpv("shaders/slim_gbuffer.frag");

        if (m_PBRVertSpv.empty() || m_PBRFragSpv.empty() || m_PBRSkinnedVertSpv.empty()
         || m_DepthPrepassVertSpv.empty() || m_DepthPrepassSkinnedVertSpv.empty()
         || m_SlimGBufferVertSpv.empty() || m_SlimGBufferSkinnedVertSpv.empty() || m_SlimGBufferFragSpv.empty())
        {
            LH_CORE_ERROR("GeometrySubsystem: shader SPIR-V empty after asset load!");
            return;
        }

        InitObjectSSBO();
        InitCullPipeline();
    }

    void GeometrySubsystem::InitObjectSSBO()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Set 5 layout: binding 0 = ObjectSSBO. BuildGPUObjectBuffer rewrites binding 0
        // each render stage against a per-frame slot — UAB no longer required.
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &binding;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_ObjectSSBODescLayout);

        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = MAX_FRAMES_IN_FLIGHT;
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets       = MAX_FRAMES_IN_FLIGHT;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_ObjectSSBODescPool);

        VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) layouts[i] = m_ObjectSSBODescLayout;
        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool     = m_ObjectSSBODescPool;
        allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        allocInfo.pSetLayouts        = layouts;
        vkAllocateDescriptorSets(device, &allocInfo, m_ObjectSSBODescSet.data());
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            char name[48]; std::snprintf(name, sizeof(name), "Geometry.ObjectSSBO.Slot%u", i);
            VulkanContext::SetDebugName(m_ObjectSSBODescSet[i], name);
        }
    }

    void GeometrySubsystem::InitCullPipeline()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Cull descriptor layout: binding 0 = ObjectSSBO (read), binding 1 = IndirectBuffer (write).
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

        // Per-frame slot of m_CullDescSet rewritten in BuildGPUObjectBuffer; cycling
        // makes the written slot disjoint from the slot the GPU is consuming.
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSize.descriptorCount = 2 * MAX_FRAMES_IN_FLIGHT;
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets       = MAX_FRAMES_IN_FLIGHT;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_CullDescPool);

        VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) layouts[i] = m_CullDescLayout;
        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool     = m_CullDescPool;
        allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        allocInfo.pSetLayouts        = layouts;
        vkAllocateDescriptorSets(device, &allocInfo, m_CullDescSet.data());
        for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            char name[48]; std::snprintf(name, sizeof(name), "Geometry.Cull.Slot%u", i);
            VulkanContext::SetDebugName(m_CullDescSet[i], name);
        }

        // PC: 6 frustum planes (96B) + objectCount + destOffset = 104B.
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset     = 0;
        pcRange.size       = sizeof(Vec4) * 6 + sizeof(u32) * 2;

        auto cullShader = ShaderLibrary::LoadEngine("shaders/gpu_cull.comp");
        auto spv = cullShader ? cullShader->GetSpirV() : std::vector<u32>{};
        if (spv.empty())
        {
            LH_CORE_ERROR("GeometrySubsystem: failed to load gpu_cull.comp!");
            return;
        }
        m_CullPipeline = std::make_unique<VKComputePipeline>(
            spv,
            std::vector<VkDescriptorSetLayout>{ m_CullDescLayout },
            std::vector<VkPushConstantRange>{ pcRange });
    }

    void GeometrySubsystem::BuildPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        BuildPBRPipelines(geoLayouts);
        BuildDepthPrepassPipelines(geoLayouts);
        BuildSlimGBufferPipelines(geoLayouts);
    }

    void GeometrySubsystem::BuildPBRPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        auto pbrLayout = MakePBRVertexLayout();
        auto bindingDescs = pbrLayout.GetBindingDescriptions();
        auto attribDescs  = pbrLayout.GetAttributeDescriptions();

        m_GeoPipelineManager.Init(geoLayouts,
            [bindingDescs, attribDescs](Material::RenderMode mode, Material::CullMode cullMode, VkPolygonMode polygonMode) -> PipelineConfig
            {
                PipelineConfig config;
                config.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_UINT };
                config.depthFormat  = VK_FORMAT_D32_SFLOAT;
                config.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                config.bindingDescriptions   = bindingDescs;
                config.attributeDescriptions = attribDescs;
                config.polygonMode = polygonMode;
                // LESS_OR_EQUAL: opaques pass DepthPrepass values (LESS) exactly;
                // cutouts/transparents Z-test against the prepass depth.
                config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

                switch (mode)
                {
                    case Material::RenderMode::Opaque:
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

        auto skinnedLayout = MakeSkinnedVertexLayout();
        auto skinnedBindingDescs = skinnedLayout.GetBindingDescriptions();
        auto skinnedAttribDescs  = skinnedLayout.GetAttributeDescriptions();

        m_GeoSkinnedPipelineManager.Init(geoLayouts,
            [skinnedBindingDescs, skinnedAttribDescs](Material::RenderMode mode, Material::CullMode cullMode, VkPolygonMode polygonMode) -> PipelineConfig
            {
                PipelineConfig config;
                config.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_UINT };
                config.depthFormat  = VK_FORMAT_D32_SFLOAT;
                config.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                config.bindingDescriptions   = skinnedBindingDescs;
                config.attributeDescriptions = skinnedAttribDescs;
                config.polygonMode    = polygonMode;
                config.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

                switch (mode)
                {
                    case Material::RenderMode::Opaque:
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

    void GeometrySubsystem::BuildDepthPrepassPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        auto [posOnlyBindings, posOnlyAttribs] = MakePositionOnlyWithFullStride();
        const auto& shadowFragSpv = m_Pipeline->GetLighting().GetShadowFragSpv();

        if (!m_DepthPrepassVertSpv.empty() && !shadowFragSpv.empty())
        {
            PipelineConfig cfg;
            cfg.colorFormats = {};
            cfg.depthFormat  = VK_FORMAT_D32_SFLOAT;
            cfg.depthTest    = true;
            cfg.depthWrite   = true;
            cfg.depthCompareOp = VK_COMPARE_OP_LESS;
            cfg.blendEnabled = false;
            cfg.cullMode     = VK_CULL_MODE_BACK_BIT;
            cfg.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            cfg.bindingDescriptions   = posOnlyBindings;
            cfg.attributeDescriptions = posOnlyAttribs;

            m_DepthPrepassPipeline = std::make_unique<VKPipeline>(
                cfg, m_DepthPrepassVertSpv, shadowFragSpv, geoLayouts);
        }

        if (!m_DepthPrepassSkinnedVertSpv.empty() && !shadowFragSpv.empty())
        {
            auto skinned = MakeSkinnedVertexLayout();
            auto skinnedBindings = skinned.GetBindingDescriptions();
            auto skinnedAttribs  = skinned.GetAttributeDescriptions();

            PipelineConfig cfg;
            cfg.colorFormats = {};
            cfg.depthFormat  = VK_FORMAT_D32_SFLOAT;
            cfg.depthTest    = true;
            cfg.depthWrite   = true;
            cfg.depthCompareOp = VK_COMPARE_OP_LESS;
            cfg.blendEnabled = false;
            cfg.cullMode     = VK_CULL_MODE_BACK_BIT;
            cfg.frontFace    = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            cfg.bindingDescriptions   = skinnedBindings;
            cfg.attributeDescriptions = skinnedAttribs;

            m_DepthPrepassSkinnedPipeline = std::make_unique<VKPipeline>(
                cfg, m_DepthPrepassSkinnedVertSpv, shadowFragSpv, geoLayouts);
        }
    }

    void GeometrySubsystem::BuildSlimGBufferPipelines(const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        // 4 color attachments mirror SlimGBufferOutput. depthFormat lets the pipeline test against
        // prepass depth via EQUAL — no depth writes (DepthPrepass owns the buffer for this frame).
        auto makeConfig = [&](auto bindings, auto attribs) {
            PipelineConfig cfg;
            cfg.colorFormats = {
                VK_FORMAT_R16G16_SFLOAT,  // SlimNormal     (octahedral)
                VK_FORMAT_R8_UNORM,       // SlimRoughness
                VK_FORMAT_R16G16_SFLOAT,  // SlimMotion     (NDC delta)
                VK_FORMAT_R16_UINT,       // SlimMaterialID
            };
            cfg.depthFormat   = VK_FORMAT_D32_SFLOAT;
            cfg.depthTest     = true;
            cfg.depthWrite    = false;                       // DepthPrepass already wrote this depth
            cfg.depthCompareOp= VK_COMPARE_OP_EQUAL;         // exact prepass match — no overdraw
            cfg.blendEnabled  = false;
            cfg.cullMode      = VK_CULL_MODE_BACK_BIT;       // CullMode::None / Wireframe deferred
            cfg.frontFace     = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            cfg.polygonMode   = VK_POLYGON_MODE_FILL;
            cfg.bindingDescriptions   = bindings;
            cfg.attributeDescriptions = attribs;
            return cfg;
        };

        // Cutout variant: same shaders, but writes its own depth (LESS_OR_EQUAL) because the opaque-only
        // prepass omits cutout — the EQUAL opaque config would reject every cutout fragment (prepass cleared
        // those pixels to 1.0 or holds the surface behind). slim_gbuffer.frag alpha-tests the holes away.
        auto makeCutoutConfig = [&](auto bindings, auto attribs) {
            auto cfg = makeConfig(bindings, attribs);
            cfg.depthWrite     = true;
            cfg.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            cfg.cullMode       = VK_CULL_MODE_NONE;   // cutout foliage is two-sided — back faces must reach
            return cfg;                               // the slim G-buffer (slim_gbuffer.frag flips the normal),
        };                                            // else RT shadows/reflections read the geometry behind it

        if (!m_SlimGBufferVertSpv.empty() && !m_SlimGBufferFragSpv.empty())
        {
            auto pbrLayout = MakePBRVertexLayout();
            auto pbrBindings = pbrLayout.GetBindingDescriptions();
            auto pbrAttribs  = pbrLayout.GetAttributeDescriptions();
            m_SlimGBufferPipeline = std::make_unique<VKPipeline>(
                makeConfig(pbrBindings, pbrAttribs),
                m_SlimGBufferVertSpv, m_SlimGBufferFragSpv, geoLayouts);
            m_SlimGBufferCutoutPipeline = std::make_unique<VKPipeline>(
                makeCutoutConfig(pbrBindings, pbrAttribs),
                m_SlimGBufferVertSpv, m_SlimGBufferFragSpv, geoLayouts);
        }
        if (!m_SlimGBufferSkinnedVertSpv.empty() && !m_SlimGBufferFragSpv.empty())
        {
            auto skinned = MakeSkinnedVertexLayout();
            auto skinnedBindings = skinned.GetBindingDescriptions();
            auto skinnedAttribs  = skinned.GetAttributeDescriptions();
            m_SlimGBufferSkinnedPipeline = std::make_unique<VKPipeline>(
                makeConfig(skinnedBindings, skinnedAttribs),
                m_SlimGBufferSkinnedVertSpv, m_SlimGBufferFragSpv, geoLayouts);
            m_SlimGBufferCutoutSkinnedPipeline = std::make_unique<VKPipeline>(
                makeCutoutConfig(skinnedBindings, skinnedAttribs),
                m_SlimGBufferSkinnedVertSpv, m_SlimGBufferFragSpv, geoLayouts);
        }
    }

    void GeometrySubsystem::Shutdown()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        m_SlimGBufferCutoutSkinnedPipeline.reset();
        m_SlimGBufferCutoutPipeline.reset();
        m_SlimGBufferSkinnedPipeline.reset();
        m_SlimGBufferPipeline.reset();
        m_DepthPrepassSkinnedPipeline.reset();
        m_DepthPrepassPipeline.reset();
        // PipelineManagers tear down internally on destruction.

        m_CullPipeline.reset();
        if (m_CullDescPool)   { vkDestroyDescriptorPool(device, m_CullDescPool, nullptr); m_CullDescPool = VK_NULL_HANDLE; }
        if (m_CullDescLayout) { vkDestroyDescriptorSetLayout(device, m_CullDescLayout, nullptr); m_CullDescLayout = VK_NULL_HANDLE; }
        m_CullDescSet.fill(VK_NULL_HANDLE);

        if (m_ObjectSSBODescPool)   { vkDestroyDescriptorPool(device, m_ObjectSSBODescPool, nullptr); m_ObjectSSBODescPool = VK_NULL_HANDLE; }
        if (m_ObjectSSBODescLayout) { vkDestroyDescriptorSetLayout(device, m_ObjectSSBODescLayout, nullptr); m_ObjectSSBODescLayout = VK_NULL_HANDLE; }
        m_ObjectSSBODescSet.fill(VK_NULL_HANDLE);
    }

    bool GeometrySubsystem::OnShaderReloaded(const std::string& name, const std::vector<u32>& spv,
                                              const std::vector<VkDescriptorSetLayout>& geoLayouts)
    {
        auto deferGfx = [](std::unique_ptr<VKPipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };
        auto deferComp = [](std::unique_ptr<VKComputePipeline>& p) {
            if (auto* raw = p.release(); raw)
                VulkanContext::Get().PushDeletion([raw]() { delete raw; });
        };

        if      (name == "pbr.vert")                   m_PBRVertSpv                 = spv;
        else if (name == "pbr.slang")                  m_PBRFragSpv                 = spv;
        else if (name == "pbr_skinned.vert")           m_PBRSkinnedVertSpv          = spv;
        else if (name == "depthPrepass.vert")          m_DepthPrepassVertSpv        = spv;
        else if (name == "depthPrepass_skinned.vert")  m_DepthPrepassSkinnedVertSpv = spv;
        else if (name == "slim_gbuffer.vert")          m_SlimGBufferVertSpv         = spv;
        else if (name == "slim_gbuffer.frag")          m_SlimGBufferFragSpv         = spv;
        else if (name == "slim_gbuffer_skinned.vert")  m_SlimGBufferSkinnedVertSpv  = spv;
        else if (name != "gpu_cull.comp") return false;

        if (name == "gpu_cull.comp" && m_CullDescLayout)
        {
            deferComp(m_CullPipeline);
            VkPushConstantRange pc{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Vec4) * 6 + sizeof(u32) * 2 };
            m_CullPipeline = std::make_unique<VKComputePipeline>(spv,
                std::vector<VkDescriptorSetLayout>{ m_CullDescLayout },
                std::vector<VkPushConstantRange>{ pc });
        }
        else if (name == "depthPrepass.vert" || name == "depthPrepass_skinned.vert")
        {
            deferGfx(m_DepthPrepassPipeline);
            deferGfx(m_DepthPrepassSkinnedPipeline);
            BuildDepthPrepassPipelines(geoLayouts);
        }
        else if (name == "slim_gbuffer.vert" || name == "slim_gbuffer.frag" || name == "slim_gbuffer_skinned.vert")
        {
            deferGfx(m_SlimGBufferPipeline);
            deferGfx(m_SlimGBufferSkinnedPipeline);
            deferGfx(m_SlimGBufferCutoutPipeline);
            deferGfx(m_SlimGBufferCutoutSkinnedPipeline);
            BuildSlimGBufferPipelines(geoLayouts);
        }
        else
        {
            // pbr.* — invalidate the pipeline manager cache, rebuild the manager.
            const bool isPBR = (name == "pbr.vert" || name == "pbr.slang");
            if (isPBR) {
                UUID pbrKey = ShaderLibrary::Get("pbr.vert")->Handle;
                m_GeoPipelineManager.DeferredInvalidateShader(pbrKey);
                m_GeoSkinnedPipelineManager.DeferredInvalidateShader(pbrKey);
            } else {
                m_GeoPipelineManager.DeferredClear();
                m_GeoSkinnedPipelineManager.DeferredClear();
            }
            BuildPBRPipelines(geoLayouts);
        }
        return true;
    }

    u32 GeometrySubsystem::EnsureMaterialRegistered(std::shared_ptr<Material> material)
    {
        auto it = m_MaterialSlotMap.find(material->Handle);
        if (it != m_MaterialSlotMap.end()) return it->second;

        u32 slot = MaterialSystem::RegisterMaterial(material);
        m_MaterialSlotMap[material->Handle] = slot;
        return slot;
    }

    void GeometrySubsystem::BuildGPUObjectBuffer(const RenderSnapshot& snapshot)
    {
        // Allocate fresh regions from the GPU tagged heap. Tag = absolute render-frame index;
        // descriptor slot = same index modulo MAX_FRAMES_IN_FLIGHT (per-frame storage rotation).
        // FreeTag(N-2) reclaims regions once the GPU retires the consuming submission.
        auto* jobCtx = JobSystem::GetCurrentJobContext();
        if (!jobCtx) return;
        const u32 frameAbs = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex());
        const u32 slot     = frameAbs % MAX_FRAMES_IN_FLIGHT;
        jobCtx->GpuCache.CurrentTag = frameAbs;

        auto& heap = Memory::GPUTaggedPageAllocator::Get();
        const u64 objBytes = static_cast<u64>(RenderPipeline::k_MaxGPUObjects) * sizeof(GPUObjectData);
        const u64 indBytes = static_cast<u64>(RenderPipeline::k_IndirectRegionCount)
                           * RenderPipeline::k_IndirectRegionStride
                           * sizeof(VkDrawIndexedIndirectCommand);

        m_ObjectRegion   = heap.Allocate(jobCtx->GpuCache, objBytes, 16);
        m_IndirectRegion = heap.Allocate(jobCtx->GpuCache, indBytes, 16);
        if (!m_ObjectRegion.buffer || !m_IndirectRegion.buffer) { m_GPUObjectCount = 0; return; }

        auto* objectData   = static_cast<GPUObjectData*>(m_ObjectRegion.mappedPtr);
        auto* indirectCmds = static_cast<VkDrawIndexedIndirectCommand*>(m_IndirectRegion.mappedPtr);
        u32   count        = 0;

        // Rebuild entity lookup. Index 0 = null sentinel; valid entities start at 1
        // (the geometry pass writes (entityID + 1) so 0 means "background").
        m_EntityLookup.clear();
        m_EntityLookup.push_back(entt::null);
        m_EntityToSSBOIndex.clear();

        for (const MeshDrawSnapshot& meshSnap : snapshot.meshes)
        {
            if (count >= RenderPipeline::k_MaxGPUObjects) break;

            auto model = AssetManager::GetAsset<Model>(meshSnap.modelUUID);
            if (!model) continue;
            const auto& meshesData = model->GetMeshesData();
            auto mesh = model->GetMesh(meshSnap.meshIndex);
            if (!mesh) continue;

            GPUObjectData& obj = objectData[count];
            obj.model = meshSnap.worldMatrix;

            // Resolve previous-frame model from the render-side cache. Newly-spawned entities
            // (cache miss) fall back to current model → zero motion for one frame.
            entt::entity entity = static_cast<entt::entity>(meshSnap.entity);
            if (auto pmIt = m_PrevModelByEntity.find(entity); pmIt != m_PrevModelByEntity.end())
                obj.prevModel = pmIt->second;
            else
                obj.prevModel = obj.model;

            const auto& aabb   = meshesData[meshSnap.meshIndex].BindPoseAABB;
            obj.boundingSphere = Vec4(aabb.Center(), Math::Length(aabb.Extents()));

            u32 matSlot = 0;
            if (meshSnap.materialUUID.IsValid()) {
                auto it = m_MaterialSlotMap.find(meshSnap.materialUUID);
                if (it != m_MaterialSlotMap.end()) matSlot = it->second;
            }
            obj.materialIndex = matSlot;
            obj.shadeMode     = static_cast<u32>(m_Pipeline->GetSystem().GetShadeMode());
            obj.entityID      = (u32)m_EntityLookup.size();
            obj.boneOffset    = meshSnap.boneOffset;

            m_EntityLookup.push_back(entity);
            m_EntityToSSBOIndex[entity] = count;

            auto* ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer()).get();
            obj.indexCount     = ib ? ib->GetCount() : 0;
            obj.firstIndex     = 0;
            obj.vertexOffset   = 0;
            // Address the dual-buffer SSBO's previous-bones region for skinned motion vectors.
            // Don't-care for non-skinned draws (their shaders never read bones[]).
            obj.prevBoneOffset = meshSnap.boneOffset + BoneMatrixBuffer::PREV_BLOCK_OFFSET;

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

        // Atomic-replace the prev-model cache from this frame's snapshot. Reads the ungated
        // snapshot list so entities with transient asset-load issues keep their prev-frame
        // matrix instead of dropping out of the cache for one frame.
        std::unordered_map<entt::entity, Mat4> nextPrev;
        nextPrev.reserve(snapshot.meshes.size());
        for (const auto& m : snapshot.meshes)
            nextPrev.emplace(static_cast<entt::entity>(m.entity), m.worldMatrix);
        m_PrevModelByEntity = std::move(nextPrev);

        heap.FlushRegion(m_ObjectRegion);
        heap.FlushRegion(m_IndirectRegion);

        VkDevice device = VulkanContext::Get().GetDevice();
        {
            VkDescriptorBufferInfo bi{};
            bi.buffer = m_ObjectRegion.buffer;
            bi.offset = m_ObjectRegion.offset;
            bi.range  = m_ObjectRegion.size;

            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet          = m_ObjectSSBODescSet[slot];
            write.dstBinding      = 0;
            write.descriptorCount = 1;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo     = &bi;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
        {
            VkDescriptorBufferInfo objInfo{};
            objInfo.buffer = m_ObjectRegion.buffer;
            objInfo.offset = m_ObjectRegion.offset;
            objInfo.range  = m_ObjectRegion.size;

            VkDescriptorBufferInfo indInfo{};
            indInfo.buffer = m_IndirectRegion.buffer;
            indInfo.offset = m_IndirectRegion.offset;
            indInfo.range  = m_IndirectRegion.size;

            VkWriteDescriptorSet writes[2]{};
            writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet          = m_CullDescSet[slot];
            writes[0].dstBinding      = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[0].pBufferInfo     = &objInfo;
            writes[1]                 = writes[0];
            writes[1].dstBinding      = 1;
            writes[1].pBufferInfo     = &indInfo;
            vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        }
    }

    void GeometrySubsystem::AddCullPass(RG::RenderGraph& rg,
                                         RG::BufferHandle objectBuffer, RG::BufferHandle indirectBuffer,
                                         const std::array<Vec4, 6>& frustumPlanes, u32 destOffset,
                                         const char* passName)
    {
        if (!m_CullPipeline || m_GPUObjectCount == 0) return;

        struct CullPassData {
            RG::BufferHandle objectBuffer;
            RG::BufferHandle indirectBuffer;
        };
        struct CullPushConstants {
            Vec4 frustumPlanes[6]; // 96B
            u32  objectCount;      // 4B
            u32  destOffset;       // 4B — index offset into commands[] (per-view-cascade region)
        };

        std::string name = passName ? passName : "FrustumCull";
        auto* pipeline = m_CullPipeline.get();
        u32 objectCount = m_GPUObjectCount;
        FrameDebugger* debugger = &m_Pipeline->GetSystem().GetFrameDebugger();

        rg.AddComputePass<CullPassData>(name,
            [=](CullPassData& data, RG::RenderPassBuilder& builder)
            {
                data.objectBuffer   = builder.ReadBuffer(objectBuffer);
                data.indirectBuffer = builder.WriteBuffer(indirectBuffer);
            },
            [this, pipeline, frustumPlanes, objectCount, destOffset, name, debugger](CullPassData&, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                if (debugger)
                    debugger->BeginCapturePass(ctx.passIndex, name, "", false,
                        { "gpu_cull", 0, 0, VK_POLYGON_MODE_FILL, false, false, false, false });

                // Recompute slot at executor time — capturing m_CullDescSet by value
                // would freeze slot 0 only (cycling refactor invariant).
                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                pipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    pipeline->GetLayout(), 0, 1, &m_CullDescSet[slot], 0, nullptr);

                CullPushConstants pc{};
                for (int i = 0; i < 6; ++i) pc.frustumPlanes[i] = frustumPlanes[i];
                pc.objectCount = objectCount;
                pc.destOffset  = destOffset;
                vkCmdPushConstants(cmd, pipeline->GetLayout(),
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPushConstants), &pc);

                u32 groupCountX = (objectCount + 255) / 256;
                vkCmdDispatch(cmd, groupCountX, 1, 1);

                if (debugger)
                {
                    debugger->CaptureComputeDispatch(name, "gpu_cull", groupCountX, 1, 1);
                    debugger->EndCapturePass();
                }
            });
    }

    RG::ResourceHandle GeometrySubsystem::AddDepthPrepass(RG::RenderGraph& rg, RG::BufferHandle indirectBufferHandle)
    {
        struct DepthPrepassData {
            RG::ResourceHandle depthTex;
            RG::BufferHandle   indirectBuf;
        };
        RG::ResourceHandle depthHandle;

        rg.AddPass<DepthPrepassData>("DepthPrepass",
            [&](DepthPrepassData& data, RG::RenderPassBuilder& builder)
            {
                const auto* view = m_Pipeline->GetCurrentView();
                RG::TextureDesc depthDesc;
                depthDesc.name   = "SceneDepth";
                depthDesc.width  = view->targets->GetSceneDepth()->GetWidth();
                depthDesc.height = view->targets->GetSceneDepth()->GetHeight();
                depthDesc.format = RG::TextureFormat::D32_Float;

                auto vkDepth = std::static_pointer_cast<VKTexture>(view->targets->GetSceneDepth());
                data.depthTex = rg.ImportResource(depthDesc,
                    (void*)vkDepth->GetImage(),
                    (void*)vkDepth->GetImageView(),
                    RG::ResourceState::Undefined);

                VkClearValue depthClear{};
                depthClear.depthStencil = { 1.0f, 0 };
                data.depthTex = builder.WriteDepth(data.depthTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depthClear);

                data.indirectBuf = builder.ReadIndirectBuffer(indirectBufferHandle);
                depthHandle = data.depthTex;
            },
            [this](DepthPrepassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "DepthPrepass", "SceneDepth", true,
                    { "depthPrepass", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, true, false });

                if (!m_DepthPrepassPipeline) { LH_CORE_ERROR("DepthPrepass pipeline is null!"); sys.GetFrameDebugger().EndCapturePass(); return; }

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_Pipeline->GetCurrentViewResources()->globalDescriptorSet[slot],
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(slot),
                    m_Pipeline->GetLighting().GetLightDescSet(slot),
                    BoneMatrixBuffer::GetDescriptorSet(slot),
                    m_ObjectSSBODescSet[slot]
                };

                m_DepthPrepassPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_DepthPrepassPipeline->GetLayout(), 0, 6, sets, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.depthTex);
                VkViewport viewport{};
                viewport.width    = (float)res->desc.width;
                viewport.height   = (float)res->desc.height;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                bool currentSkinned = false;

                // Opaque-only: cutouts/transparents write their depth in GeometryPass.
                for (const auto& dc : sys.GetDrawList().opaque)
                {
                    auto mesh = dc.model->GetMesh(dc.meshIndex);
                    auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                    auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                    if (!vb || !ib) continue;

                    if (dc.isSkinned != currentSkinned)
                    {
                        currentSkinned = dc.isSkinned;
                        if (currentSkinned && m_DepthPrepassSkinnedPipeline)
                        {
                            m_DepthPrepassSkinnedPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_DepthPrepassSkinnedPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                        }
                        else
                        {
                            m_DepthPrepassPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_DepthPrepassPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                        }
                    }

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                    const u32 viewBaseRegion = m_Pipeline->GetCurrentView()->viewIndex * RenderPipeline::k_IndirectRegionsPerView;
                    const u32 cmdIndex = viewBaseRegion * RenderPipeline::k_IndirectRegionStride + dc.gpuObjectIndex;
                    VkDeviceSize indirectOffset = m_IndirectRegion.offset + cmdIndex * sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cmd, m_IndirectRegion.buffer, indirectOffset, 1,
                        sizeof(VkDrawIndexedIndirectCommand));

                    if (sys.GetFrameDebugger().state == DebuggerState::CaptureRequested)
                    {
                        std::string entName = "Entity";
                        const auto& tags = sys.GetActiveSnapshot().tagsByEntity;
                        u32 idx = entt::to_entity(dc.entity);
                        if (idx < tags.size() && tags[idx])
                            entName = tags[idx];
                        sys.GetFrameDebugger().CaptureIndirectDraw("DepthPrepass",
                            dc.model->GetName() + "[" + std::to_string(dc.meshIndex) + "]",
                            entName, dc.entityIndex, ib->GetCount(), dc.gpuObjectIndex, indirectOffset,
                            { "depthPrepass", 0, static_cast<u32>(VK_CULL_MODE_BACK_BIT),
                              VK_POLYGON_MODE_FILL, dc.isSkinned, true, true, false });
                    }
                }

                sys.GetFrameDebugger().EndCapturePass();
            }
        );

        return depthHandle;
    }

    SlimGBufferOutput GeometrySubsystem::AddSlimGBufferPass(RG::RenderGraph& rg,
                                                            RG::BufferHandle indirectBufferHandle,
                                                            RG::ResourceHandle sceneDepth)
    {
        struct SlimGBufferData {
            RG::ResourceHandle normalTex;
            RG::ResourceHandle roughnessTex;
            RG::ResourceHandle motionTex;
            RG::ResourceHandle materialIDTex;
            RG::ResourceHandle depthTex;
            RG::BufferHandle   indirectBuf;
        };
        SlimGBufferOutput output;

        rg.AddPass<SlimGBufferData>("SlimGBufferPass",
            [&, sceneDepth](SlimGBufferData& data, RG::RenderPassBuilder& builder)
            {
                const auto* view = m_Pipeline->GetCurrentView();
                const u32 w = view->targets->GetSlimNormal()->GetWidth();
                const u32 h = view->targets->GetSlimNormal()->GetHeight();

                auto importColor = [&](const std::shared_ptr<Texture>& tex,
                                       const char* name, RG::TextureFormat fmt) -> RG::ResourceHandle
                {
                    RG::TextureDesc desc;
                    desc.name = name; desc.width = w; desc.height = h; desc.format = fmt;
                    auto vkTex = std::static_pointer_cast<VKTexture>(tex);
                    return rg.ImportResource(desc,
                        (void*)vkTex->GetImage(), (void*)vkTex->GetImageView(),
                        RG::ResourceState::Undefined);
                };

                data.normalTex     = importColor(view->targets->GetSlimNormal(),     "SlimNormal",     RG::TextureFormat::RG16_Float);
                data.roughnessTex  = importColor(view->targets->GetSlimRoughness(),  "SlimRoughness",  RG::TextureFormat::R8_Unorm);
                data.motionTex     = importColor(view->targets->GetSlimMotion(),     "SlimMotion",     RG::TextureFormat::RG16_Float);
                data.materialIDTex = importColor(view->targets->GetSlimMaterialID(), "SlimMaterialID", RG::TextureFormat::R16_Uint);

                // Clear values: encoded up-vector for normal, max roughness, zero motion, null matID.
                VkClearValue normalClear{};     normalClear.color.float32[0] = 0.5f; normalClear.color.float32[1] = 0.5f;
                VkClearValue roughnessClear{};  roughnessClear.color.float32[0] = 1.0f;
                VkClearValue motionClear{};     motionClear.color.float32[0] = 0.0f; motionClear.color.float32[1] = 0.0f;
                VkClearValue materialIDClear{}; materialIDClear.color.uint32[0] = 0u;

                data.normalTex     = builder.Write(data.normalTex,     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, normalClear);
                data.roughnessTex  = builder.Write(data.roughnessTex,  VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, roughnessClear);
                data.motionTex     = builder.Write(data.motionTex,     VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, motionClear);
                data.materialIDTex = builder.Write(data.materialIDTex, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, materialIDClear);

                // Depth: LOAD prepass depth, keep storing (downstream GTAO/Geometry passes still
                // need it). EQUAL test in the pipeline; depthWrite=false → SceneDepth contents
                // are preserved bit-for-bit.
                data.depthTex    = builder.WriteDepth(sceneDepth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {});
                data.indirectBuf = builder.ReadIndirectBuffer(indirectBufferHandle);

                output.normal     = data.normalTex;
                output.roughness  = data.roughnessTex;
                output.motion     = data.motionTex;
                output.materialID = data.materialIDTex;
            },
            [this](SlimGBufferData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();

                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "SlimGBufferPass", "SlimNormal", false,
                    { "slim_gbuffer", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, false, false });

                if (!m_SlimGBufferPipeline) { LH_CORE_ERROR("SlimGBuffer pipeline is null!"); sys.GetFrameDebugger().EndCapturePass(); return; }

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_Pipeline->GetCurrentViewResources()->globalDescriptorSet[slot],
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(slot),
                    m_Pipeline->GetLighting().GetLightDescSet(slot),
                    BoneMatrixBuffer::GetDescriptorSet(slot),
                    m_ObjectSSBODescSet[slot]
                };

                m_SlimGBufferPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_SlimGBufferPipeline->GetLayout(), 0, 6, sets, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.normalTex);
                VkViewport viewport{};
                viewport.width    = (float)res->desc.width;
                viewport.height   = (float)res->desc.height;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                bool currentSkinned = false;

                // Opaque pass — EQUAL against prepass depth, no depth write. Cutout follows in its own
                // loop below (writes depth + alpha-tests). See arch/rendering-pipeline.md.
                for (const auto& dc : sys.GetDrawList().opaque)
                {
                    auto mesh = dc.model->GetMesh(dc.meshIndex);
                    auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                    auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                    if (!vb || !ib) continue;

                    if (dc.isSkinned != currentSkinned)
                    {
                        currentSkinned = dc.isSkinned;
                        if (currentSkinned && m_SlimGBufferSkinnedPipeline)
                        {
                            m_SlimGBufferSkinnedPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_SlimGBufferSkinnedPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                        }
                        else
                        {
                            m_SlimGBufferPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_SlimGBufferPipeline->GetLayout(), 0, 6, sets, 0, nullptr);
                        }
                    }

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                    const u32 viewBaseRegion = m_Pipeline->GetCurrentView()->viewIndex * RenderPipeline::k_IndirectRegionsPerView;
                    const u32 cmdIndex = viewBaseRegion * RenderPipeline::k_IndirectRegionStride + dc.gpuObjectIndex;
                    VkDeviceSize indirectOffset = m_IndirectRegion.offset + cmdIndex * sizeof(VkDrawIndexedIndirectCommand);
                    vkCmdDrawIndexedIndirect(cmd, m_IndirectRegion.buffer, indirectOffset, 1,
                        sizeof(VkDrawIndexedIndirectCommand));

                    if (sys.GetFrameDebugger().state == DebuggerState::CaptureRequested)
                    {
                        std::string entName = "Entity";
                        const auto& tags = sys.GetActiveSnapshot().tagsByEntity;
                        u32 idx = entt::to_entity(dc.entity);
                        if (idx < tags.size() && tags[idx])
                            entName = tags[idx];
                        sys.GetFrameDebugger().CaptureIndirectDraw("SlimGBufferPass",
                            dc.model->GetName() + "[" + std::to_string(dc.meshIndex) + "]",
                            entName, dc.entityIndex, ib->GetCount(), dc.gpuObjectIndex, indirectOffset,
                            { "slim_gbuffer", 0, static_cast<u32>(VK_CULL_MODE_BACK_BIT),
                              VK_POLYGON_MODE_FILL, dc.isSkinned, true, false, false });
                    }
                }

                // Cutout — alpha-tested into the slim G-buffer as alpha-tested-opaque. Writes its own
                // depth (LESS_OR_EQUAL) so SceneDepth + SlimNormal carry the holed surface; RT sun shadows /
                // reflections + GTAO then reconstruct from it instead of the geometry behind the holes.
                if (m_SlimGBufferCutoutPipeline && !sys.GetDrawList().cutout.empty())
                {
                    currentSkinned = false;
                    m_SlimGBufferCutoutPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_SlimGBufferCutoutPipeline->GetLayout(), 0, 6, sets, 0, nullptr);

                    for (const auto& dc : sys.GetDrawList().cutout)
                    {
                        auto mesh = dc.model->GetMesh(dc.meshIndex);
                        auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                        auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                        if (!vb || !ib) continue;

                        if (dc.isSkinned != currentSkinned)
                        {
                            currentSkinned = dc.isSkinned;
                            VKPipeline* p = (currentSkinned && m_SlimGBufferCutoutSkinnedPipeline)
                                ? m_SlimGBufferCutoutSkinnedPipeline.get()
                                : m_SlimGBufferCutoutPipeline.get();
                            p->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                p->GetLayout(), 0, 6, sets, 0, nullptr);
                        }

                        VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                        VkDeviceSize offsets[] = { 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                        vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                        const u32 viewBaseRegion = m_Pipeline->GetCurrentView()->viewIndex * RenderPipeline::k_IndirectRegionsPerView;
                        const u32 cmdIndex = viewBaseRegion * RenderPipeline::k_IndirectRegionStride + dc.gpuObjectIndex;
                        VkDeviceSize indirectOffset = m_IndirectRegion.offset + cmdIndex * sizeof(VkDrawIndexedIndirectCommand);
                        vkCmdDrawIndexedIndirect(cmd, m_IndirectRegion.buffer, indirectOffset, 1,
                            sizeof(VkDrawIndexedIndirectCommand));

                        if (sys.GetFrameDebugger().state == DebuggerState::CaptureRequested)
                        {
                            std::string entName = "Entity";
                            const auto& tags = sys.GetActiveSnapshot().tagsByEntity;
                            u32 idx = entt::to_entity(dc.entity);
                            if (idx < tags.size() && tags[idx])
                                entName = tags[idx];
                            sys.GetFrameDebugger().CaptureIndirectDraw("SlimGBufferPass",
                                dc.model->GetName() + "[" + std::to_string(dc.meshIndex) + "]",
                                entName, dc.entityIndex, ib->GetCount(), dc.gpuObjectIndex, indirectOffset,
                                { "slim_gbuffer", 0, static_cast<u32>(VK_CULL_MODE_BACK_BIT),
                                  VK_POLYGON_MODE_FILL, dc.isSkinned, true, true, false });
                        }
                    }
                }

                sys.GetFrameDebugger().EndCapturePass();
            }
        );

        return output;
    }

    GeometryOutput GeometrySubsystem::AddGeometryPass(RG::RenderGraph& rg,
                                                      const RG::ResourceHandle (&shadowHandles)[k_ShadowCascadeCount],
                                                      RG::BufferHandle indirectBufferHandle,
                                                      RG::ResourceHandle sceneDepth,
                                                      RG::ResourceHandle gtaoFinalAO,
                                                      RG::ResourceHandle rtShadowMask,
                                                      RG::ResourceHandle diHandle,
                                                      RG::ResourceHandle giDIHandle,
                                                      RG::ResourceHandle reflHandle,
                                                      RG::ResourceHandle diSpecHandle)
    {
        struct GeometryPassData {
            RG::ResourceHandle outputTex;
            RG::ResourceHandle entityIDTex;
            RG::ResourceHandle depthTex;
            RG::ResourceHandle shadowCascades[k_ShadowCascadeCount];
            RG::ResourceHandle gtaoFinalAO;
            RG::ResourceHandle rtShadowMask;
            RG::ResourceHandle diHandle;
            RG::ResourceHandle giDIHandle;
            RG::ResourceHandle reflHandle;
            RG::ResourceHandle diSpecHandle;
            RG::BufferHandle   indirectBuf;
        };
        GeometryOutput output;

        rg.AddPass<GeometryPassData>("GeometryPass",
            [&, sceneDepth](GeometryPassData& data, RG::RenderPassBuilder& builder)
            {
                const auto* view = m_Pipeline->GetCurrentView();
                RG::TextureDesc desc;
                desc.name   = "SceneColor";
                desc.width  = view->targets->GetSceneColor()->GetWidth();
                desc.height = view->targets->GetSceneColor()->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;

                auto vkTex = std::static_pointer_cast<VKTexture>(view->targets->GetSceneColor());
                data.outputTex = rg.ImportResource(desc,
                    (void*)vkTex->GetImage(),
                    (void*)vkTex->GetImageView(),
                    RG::ResourceState::ShaderResource);

                RG::TextureDesc idDesc;
                idDesc.name   = "EntityID";
                idDesc.width  = view->targets->GetEntityIDBuffer()->GetWidth();
                idDesc.height = view->targets->GetEntityIDBuffer()->GetHeight();
                idDesc.format = RG::TextureFormat::R32_Uint;

                auto vkID = std::static_pointer_cast<VKTexture>(view->targets->GetEntityIDBuffer());
                data.entityIDTex = rg.ImportResource(idDesc,
                    (void*)vkID->GetImage(),
                    (void*)vkID->GetImageView(),
                    RG::ResourceState::Undefined);

                // SceneDepth is produced by DepthPrepass — load + keep writing (cutouts
                // still write their own depth; opaques pass LESS_EQUAL against prepass).
                data.depthTex  = builder.WriteDepth(sceneDepth,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {});
                data.outputTex = builder.Write(data.outputTex);

                VkClearValue idClear{};
                idClear.color.uint32[0] = 0;
                data.entityIDTex = builder.Write(data.entityIDTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, idClear);

                // Per-cascade Read triggers DEPTH→SHADER_READ barriers (baseArrayLayer=i, layerCount=1).
                for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                    if (shadowHandles[i].IsValid())
                        data.shadowCascades[i] = builder.Read(shadowHandles[i]);

                // pbr.frag samples gtaoFinal via Set 0 binding 4 — explicit Read triggers
                // GENERAL → SHADER_READ_ONLY transition from GTAODenoise.
                if (gtaoFinalAO.IsValid())
                    data.gtaoFinalAO = builder.Read(gtaoFinalAO);

                // RT sun-shadow mask. Read triggers GENERAL (RT raygen storage write) →
                // SHADER_READ_ONLY transition. Handle is invalid in CSM mode (RtSubsystem
                // returns {} when the pass is gated off) — pbr.frag's CSM branch doesn't
                // dynamically access binding 4, so the descriptor's layout is irrelevant.
                if (rtShadowMask.IsValid())
                    data.rtShadowMask = builder.Read(rtShadowMask);

                // ReSTIR DI image — Read triggers GENERAL (RestirShade storage write) →
                // SHADER_READ_ONLY transition before pbr.frag samples it at Set 3 b5. Invalid
                // handle when ReSTIR is off / no TLAS; pbr.frag's restirParams.x gate then keeps the
                // descriptor untouched and runs the point loop instead.
                if (diHandle.IsValid())
                    data.diHandle = builder.Read(diHandle);

                // ReSTIR GI image — same GENERAL → SHADER_READ_ONLY transition as the DI handle,
                // before pbr.frag samples it at Set 3 b6. Invalid when GI is off / no TLAS; the
                // restirParams.y gate then keeps the descriptor untouched.
                if (giDIHandle.IsValid())
                    data.giDIHandle = builder.Read(giDIHandle);

                // Denoised RT reflection radiance (D.1) — keeps the reflection trace + specular denoiser
                // alive (no pbr.frag sampler until the Set 3 b7 composite, S4). Read-only dependency.
                if (reflHandle.IsValid())
                    data.reflHandle = builder.Read(reflHandle);

                // Denoised ReSTIR-DI specular (#154) — barrier-only read; pbr.frag samples it at Set 3 b8
                // under the restirParams.z gate. Invalid when DI / specular off.
                if (diSpecHandle.IsValid())
                    data.diSpecHandle = builder.Read(diSpecHandle);

                data.indirectBuf = builder.ReadIndirectBuffer(indirectBufferHandle);

                output.color    = data.outputTex;
                output.depth    = data.depthTex;
                output.entityID = data.entityIDTex;
            },
            [this](GeometryPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                auto& sys = m_Pipeline->GetSystem();

                VkPolygonMode polyMode = (sys.GetShadeMode() == ShadeMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
                sys.GetFrameDebugger().BeginCapturePass(ctx.passIndex, "GeometryPass", "SceneColor", false,
                    { "pbr", 0, VK_CULL_MODE_BACK_BIT, polyMode, false, true, true, false });

                UUID pbrUUID = ShaderLibrary::Get("pbr.vert")->Handle;
                auto* opaquePipeline = m_GeoPipelineManager.GetOrCreate(
                    pbrUUID, Material::RenderMode::Opaque, Material::CullMode::Back, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                if (!opaquePipeline) { sys.GetFrameDebugger().EndCapturePass(); return; }
                VkPipelineLayout pipelineLayout = opaquePipeline->GetLayout();

                const u32 slot = static_cast<u32>(Renderer::GetFrameData()->GetRenderFrameIndex()) % MAX_FRAMES_IN_FLIGHT;
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_Pipeline->GetCurrentViewResources()->globalDescriptorSet[slot],
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(slot),
                    m_Pipeline->GetLighting().GetLightDescSet(slot),
                    BoneMatrixBuffer::GetDescriptorSet(slot),
                    m_ObjectSSBODescSet[slot]
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout, 0, 6, sets, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.outputTex);
                VkViewport viewport{};
                viewport.width    = (float)res->desc.width;
                viewport.height   = (float)res->desc.height;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                auto DrawBatch = [&](const std::vector<DrawCommand>& draws, Material::RenderMode mode)
                {
                    if (draws.empty()) return;

                    Material::CullMode currentCull = Material::CullMode::Back;
                    bool currentSkinned = false;
                    auto* pipeline = m_GeoPipelineManager.GetOrCreate(
                        pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                    if (!pipeline) return;
                    pipeline->Bind(cmd);

                    for (const auto& dc : draws)
                    {
                        if (dc.cullMode != currentCull || dc.isSkinned != currentSkinned)
                        {
                            currentCull    = dc.cullMode;
                            currentSkinned = dc.isSkinned;
                            VKPipeline* newPipeline = currentSkinned
                                ? m_GeoSkinnedPipelineManager.GetOrCreate(pbrUUID, mode, currentCull, polyMode, m_PBRSkinnedVertSpv, m_PBRFragSpv)
                                : m_GeoPipelineManager.GetOrCreate       (pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv,        m_PBRFragSpv);
                            if (!newPipeline) continue;
                            newPipeline->Bind(cmd);
                        }

                        auto mesh = dc.model->GetMesh(dc.meshIndex);
                        auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                        auto ib = std::static_pointer_cast<VKIndexBuffer >(mesh->GetIndexBuffer ());
                        if (!vb || !ib) continue;

                        VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                        VkDeviceSize offsets[] = { 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                        vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                        // GPU cull sets instanceCount=0 for culled draws. gl_BaseInstance =
                        // dc.gpuObjectIndex; shader reads objects[gl_BaseInstance] via Set 5.
                        const u32 viewBaseRegion = m_Pipeline->GetCurrentView()->viewIndex * RenderPipeline::k_IndirectRegionsPerView;
                        const u32 cmdIndex = viewBaseRegion * RenderPipeline::k_IndirectRegionStride + dc.gpuObjectIndex;
                        VkDeviceSize indirectOffset = m_IndirectRegion.offset + cmdIndex * sizeof(VkDrawIndexedIndirectCommand);
                        vkCmdDrawIndexedIndirect(cmd, m_IndirectRegion.buffer, indirectOffset, 1,
                            sizeof(VkDrawIndexedIndirectCommand));

                        if (sys.GetFrameDebugger().state == DebuggerState::CaptureRequested)
                        {
                            std::string entName = "Entity";
                            const auto& tags = sys.GetActiveSnapshot().tagsByEntity;
                            u32 idx = entt::to_entity(dc.entity);
                            if (idx < tags.size() && tags[idx])
                                entName = tags[idx];
                            u32 vkCull = (currentCull == Material::CullMode::Back) ? VK_CULL_MODE_BACK_BIT
                                       : (currentCull == Material::CullMode::Front) ? VK_CULL_MODE_FRONT_BIT
                                       : VK_CULL_MODE_NONE;
                            sys.GetFrameDebugger().CaptureIndirectDraw("GeometryPass",
                                dc.model->GetName() + "[" + std::to_string(dc.meshIndex) + "]",
                                entName, dc.entityIndex, ib->GetCount(),
                                dc.gpuObjectIndex, indirectOffset,
                                { "pbr", static_cast<u32>(mode), vkCull, polyMode, currentSkinned, true, true,
                                  mode == Material::RenderMode::Transparent || mode == Material::RenderMode::Fade });
                        }
                    }
                };

                // Transparent moved to TransparencySubsystem's pass after skybox + fog composite.
                DrawBatch(sys.GetDrawList().opaque, Material::RenderMode::Opaque);
                DrawBatch(sys.GetDrawList().cutout, Material::RenderMode::Cutout);

                sys.GetFrameDebugger().EndCapturePass();
            }
        );
        return output;
    }
}
