#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/MaterialSystem.h"
#include "luth/renderer/BoneMatrixBuffer.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/Buffer.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/renderer/ShaderCompiler.h"
#include "luth/renderer/IBLPrecompute.h"
#include "luth/renderer/DrawCommand.h"
#include "luth/renderer/passes/CullPass.h"
#include "luth/renderer/backend/vulkan/VulkanAllocator.h"
#include "luth/renderer/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/VulkanShader.h"
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <vma/vk_mem_alloc.h>

namespace Luth
{
    using namespace Component;

    // =========================================================================
    // Construction / Destruction
    // =========================================================================

    RenderingSystem::RenderingSystem(u32 viewportWidth, u32 viewportHeight)
    {
        m_FrameAllocator = std::make_unique<Memory::LinearAllocator>(1 * Memory::MB);

        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan)
        {
            m_SceneColor     = Texture::Create(viewportWidth, viewportHeight, TextureFormat::RGBA16F);
            m_SceneDepth     = Texture::Create(viewportWidth, viewportHeight, TextureFormat::D32_Float);
            m_LDROutput      = Texture::Create(viewportWidth, viewportHeight, TextureFormat::RGBA8);
            m_EntityIDBuffer = Texture::Create(viewportWidth, viewportHeight, TextureFormat::R32_Uint);
            m_SelectionMask  = Texture::Create(viewportWidth, viewportHeight, TextureFormat::RGBA8);
            m_SelectionDepth = Texture::Create(viewportWidth, viewportHeight, TextureFormat::D32_Float);

            InitGlobalUniforms();
            InitShadowResources();

            // Load shaders via AssetManager (imports + caches SPIR-V on first run)
            UUID pbrUUID = AssetDatabase::GetUUID(FileSystem::EngineAssetsPath("shaders/pbr.vert"));
            auto pbrShader = std::static_pointer_cast<Shader>(AssetManager::LoadImmediate(pbrUUID));
            if (!pbrShader)
            {
                LH_CORE_ERROR("Failed to load PBR shader via AssetManager!");
                return;
            }
            ShaderLibrary::Register("pbr", pbrShader);

            UUID shadowUUID = AssetDatabase::GetUUID(FileSystem::EngineAssetsPath("shaders/shadowDepth.vert"));
            auto shadowShader = std::static_pointer_cast<Shader>(AssetManager::LoadImmediate(shadowUUID));
            if (!shadowShader)
            {
                LH_CORE_ERROR("Failed to load shadow shader via AssetManager!");
                return;
            }
            ShaderLibrary::Register("shadowDepth", shadowShader);

            // Extract SPIR-V for pipeline creation
            auto vkPbr = std::static_pointer_cast<VulkanShader>(pbrShader);
            m_PBRVertSpv = vkPbr->GetSpirV(VK_SHADER_STAGE_VERTEX_BIT);
            m_PBRFragSpv = vkPbr->GetSpirV(VK_SHADER_STAGE_FRAGMENT_BIT);

            if (m_PBRVertSpv.empty() || m_PBRFragSpv.empty())
            {
                LH_CORE_ERROR("Failed to compile PBR shaders!");
                return;
            }

            auto vkShadow = std::static_pointer_cast<VulkanShader>(shadowShader);
            m_ShadowVertSpv = vkShadow->GetSpirV(VK_SHADER_STAGE_VERTEX_BIT);
            m_ShadowFragSpv = vkShadow->GetSpirV(VK_SHADER_STAGE_FRAGMENT_BIT);

            if (m_ShadowVertSpv.empty() || m_ShadowFragSpv.empty())
            {
                LH_CORE_ERROR("Failed to compile shadow shaders!");
                return;
            }

            // Load skinned vertex shaders via ShaderCompiler
            {
                auto shadersPath = FileSystem::EngineAssetsPath("shaders");
                m_PBRSkinnedVertSpv            = ShaderCompiler::Compile(shadersPath / "pbr_skinned.vert");
                m_ShadowSkinnedVertSpv         = ShaderCompiler::Compile(shadersPath / "shadowDepth_skinned.vert");
                m_SelectionMaskVertSpv         = ShaderCompiler::Compile(shadersPath / "selectionMask.vert");
                m_SelectionMaskFragSpv         = ShaderCompiler::Compile(shadersPath / "selectionMask.frag");
                m_SelectionMaskSkinnedVertSpv  = ShaderCompiler::Compile(shadersPath / "selectionMask_skinned.vert");

                if (m_PBRSkinnedVertSpv.empty() || m_ShadowSkinnedVertSpv.empty())
                    LH_CORE_ERROR("Failed to compile skinned shaders!");
                if (m_SelectionMaskVertSpv.empty() || m_SelectionMaskFragSpv.empty())
                    LH_CORE_ERROR("Failed to compile selection mask shaders!");
            }

            // Load post-process shaders via ShaderCompiler (not asset pipeline — inline shaders)
            {
                auto shadersPath = FileSystem::EngineAssetsPath("shaders");
                m_FullscreenVertSpv    = ShaderCompiler::Compile(shadersPath / "fullscreen.vert");
                m_BloomExtractFragSpv  = ShaderCompiler::Compile(shadersPath / "bloomExtract.frag");
                m_BloomBlurFragSpv     = ShaderCompiler::Compile(shadersPath / "bloomBlur.frag");
                m_PostProcessFragSpv   = ShaderCompiler::Compile(shadersPath / "postprocess.frag");
                m_OutlineFragSpv       = ShaderCompiler::Compile(shadersPath / "outline.frag");
                m_GridFragSpv          = ShaderCompiler::Compile(shadersPath / "grid.frag");

                if (m_FullscreenVertSpv.empty() || m_BloomExtractFragSpv.empty() ||
                    m_BloomBlurFragSpv.empty() || m_PostProcessFragSpv.empty() ||
                    m_OutlineFragSpv.empty() || m_GridFragSpv.empty())
                {
                    LH_CORE_ERROR("Failed to compile post-process shaders!");
                }
            }

            BoneMatrixBuffer::Init();
            InitPostProcessResources();
            InitIBLResources(FileSystem::ResolveAsset("textures/environment.hdr"));
            CreatePipelines();
            InitGPUObjectBuffers();
            InitCullPipeline();

            // Shader reload callback — rebuilds pipelines when a shader is reloaded
            ShaderLibrary::SetReloadCallback([this](const std::string& name) {
                vkDeviceWaitIdle(VulkanContext::Get().GetDevice());

                if (name == "pbr") {
                    auto vk = std::static_pointer_cast<VulkanShader>(ShaderLibrary::Get("pbr"));
                    m_PBRVertSpv = vk->GetSpirV(VK_SHADER_STAGE_VERTEX_BIT);
                    m_PBRFragSpv = vk->GetSpirV(VK_SHADER_STAGE_FRAGMENT_BIT);
                } else if (name == "shadowDepth") {
                    auto vk = std::static_pointer_cast<VulkanShader>(ShaderLibrary::Get("shadowDepth"));
                    m_ShadowVertSpv = vk->GetSpirV(VK_SHADER_STAGE_VERTEX_BIT);
                    m_ShadowFragSpv = vk->GetSpirV(VK_SHADER_STAGE_FRAGMENT_BIT);
                }

                if (name == "pbr") {
                    m_GeoPipelineManager.InvalidateShader(ShaderLibrary::Get("pbr")->Handle);
                    m_GeoSkinnedPipelineManager.InvalidateShader(ShaderLibrary::Get("pbr")->Handle);
                } else {
                    m_GeoPipelineManager.Clear();
                    m_GeoSkinnedPipelineManager.Clear();
                }
                m_ShadowPipeline.reset();
                m_ShadowSkinnedPipeline.reset();
                m_SkyboxPipeline.reset();
                m_BloomExtractPipeline.reset();
                m_BloomBlurPipeline.reset();
                m_PostProcessPipeline.reset();
                m_OutlinePipeline.reset();
                m_GridPipeline.reset();
                m_SelectionMaskPipeline.reset();
                m_SelectionMaskSkinnedPipeline.reset();
                CreatePipelines();
                LH_CORE_INFO("Pipelines rebuilt after shader reload: {}", name);
            });

            // File watcher for shader hot-reload
            m_ShaderWatcher.AddWatch(FileSystem::EngineAssetsPath("shaders"));
            m_ShaderWatcher.SetCallback([this](const fs::path& changedFile, FileWatcher::FileStatus status) {
                if (status != FileWatcher::FileStatus::Modified) return;

                std::string ext = changedFile.extension().string();
                if (ext != ".vert" && ext != ".frag") return;

                std::string stem = changedFile.stem().string();
                bool matched = false;
                for (const auto& [name, shader] : ShaderLibrary::GetAll()) {
                    if (shader->GetPath().stem().string() == stem) {
                        std::lock_guard lock(m_ReloadMutex);
                        m_PendingReloads.insert(name);
                        matched = true;
                        break;
                    }
                }

                if (!matched) {
                    std::lock_guard lock(m_ReloadMutex);
                    m_PendingUtilityReload = true;
                }
            });
            m_ShaderWatcher.Start(true);

            m_GPUTimers.Init(16);
            RegisterNamedTextures();
        }
    }

    RenderingSystem::~RenderingSystem()
    {
        m_ShaderWatcher.Stop();
        ShaderLibrary::SetReloadCallback(nullptr);
        m_GPUTimers.Shutdown();

        // Bone blocks now owned by AnimationSystem — just shut down the buffer
        BoneMatrixBuffer::Shutdown();

        VkDevice device = VulkanContext::Get().GetDevice();

        m_FrameDebugger.Shutdown(device);

        if (m_OutlineSampler)
            vkDestroySampler(device, m_OutlineSampler, nullptr);
        if (m_OutlineDescSetLayout)
            vkDestroyDescriptorSetLayout(device, m_OutlineDescSetLayout, nullptr);
        if (m_OutlineDescPool)
            vkDestroyDescriptorPool(device, m_OutlineDescPool, nullptr);
        if (m_GridDepthSampler)
            vkDestroySampler(device, m_GridDepthSampler, nullptr);
        if (m_GridDescSetLayout)
            vkDestroyDescriptorSetLayout(device, m_GridDescSetLayout, nullptr);
        if (m_GridDescPool)
            vkDestroyDescriptorPool(device, m_GridDescPool, nullptr);
        if (m_PPSampler)
            vkDestroySampler(device, m_PPSampler, nullptr);
        if (m_PPDescSetLayout)
            vkDestroyDescriptorSetLayout(device, m_PPDescSetLayout, nullptr);
        if (m_PPDescPool)
            vkDestroyDescriptorPool(device, m_PPDescPool, nullptr);
        if (m_IBLSampler)
            vkDestroySampler(device, m_IBLSampler, nullptr);
        if (m_ShadowSampler)
            vkDestroySampler(device, m_ShadowSampler, nullptr);
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
        {
            if (m_ShadowLayerViews[i])
                vkDestroyImageView(device, m_ShadowLayerViews[i], nullptr);
        }
        if (m_LightSetLayout)
            vkDestroyDescriptorSetLayout(device, m_LightSetLayout, nullptr);
        if (m_LightDescPool)
            vkDestroyDescriptorPool(device, m_LightDescPool, nullptr);
        if (m_GlobalSetLayout)
            vkDestroyDescriptorSetLayout(device, m_GlobalSetLayout, nullptr);

        if (m_ObjectSSBO)
        {
            VulkanAllocator::Unmap(m_ObjectSSBOAlloc);
            VulkanAllocator::FreeBuffer(m_ObjectSSBO, m_ObjectSSBOAlloc);
        }
        if (m_IndirectBuffer)
        {
            VulkanAllocator::Unmap(m_IndirectBufferAlloc);
            VulkanAllocator::FreeBuffer(m_IndirectBuffer, m_IndirectBufferAlloc);
        }
        if (m_ObjectSSBODescPool)
            vkDestroyDescriptorPool(device, m_ObjectSSBODescPool, nullptr);
        if (m_ObjectSSBODescLayout)
            vkDestroyDescriptorSetLayout(device, m_ObjectSSBODescLayout, nullptr);
        if (m_CullDescLayout)
            vkDestroyDescriptorSetLayout(device, m_CullDescLayout, nullptr);
        m_CullPipeline.reset();
    }

    // =========================================================================
    // Project lifecycle
    // =========================================================================

    void RenderingSystem::OnProjectLoaded()
    {
        // Add the project's shaders dir (if it exists) to the hot-reload watcher
        // alongside the engine shader dir registered in the constructor.
        if (!FileSystem::HasProject()) return;

        fs::path projectShaders = FileSystem::AssetsPath("shaders");
        if (!fs::exists(projectShaders) || !fs::is_directory(projectShaders))
            return;

        m_ShaderWatcher.AddWatch(projectShaders);
        m_WatchedProjectShaderDir = projectShaders;
        LH_CORE_INFO("Shader hot-reload watching project dir: {}", projectShaders.string());
    }

    void RenderingSystem::OnProjectUnloaded()
    {
        if (m_WatchedProjectShaderDir.empty()) return;
        m_ShaderWatcher.RemoveWatch(m_WatchedProjectShaderDir);
        m_WatchedProjectShaderDir.clear();
    }

    void RenderingSystem::RecompileUtilityShaders()
    {
        auto shadersPath = FileSystem::EngineAssetsPath("shaders");

        m_PBRSkinnedVertSpv           = ShaderCompiler::Compile(shadersPath / "pbr_skinned.vert");
        m_ShadowSkinnedVertSpv        = ShaderCompiler::Compile(shadersPath / "shadowDepth_skinned.vert");
        m_SelectionMaskVertSpv        = ShaderCompiler::Compile(shadersPath / "selectionMask.vert");
        m_SelectionMaskFragSpv        = ShaderCompiler::Compile(shadersPath / "selectionMask.frag");
        m_SelectionMaskSkinnedVertSpv = ShaderCompiler::Compile(shadersPath / "selectionMask_skinned.vert");

        m_FullscreenVertSpv   = ShaderCompiler::Compile(shadersPath / "fullscreen.vert");
        m_BloomExtractFragSpv = ShaderCompiler::Compile(shadersPath / "bloomExtract.frag");
        m_BloomBlurFragSpv    = ShaderCompiler::Compile(shadersPath / "bloomBlur.frag");
        m_PostProcessFragSpv  = ShaderCompiler::Compile(shadersPath / "postprocess.frag");
        m_OutlineFragSpv      = ShaderCompiler::Compile(shadersPath / "outline.frag");
        m_GridFragSpv         = ShaderCompiler::Compile(shadersPath / "grid.frag");

        m_SkyboxVertSpv = ShaderCompiler::Compile(shadersPath / "skybox.vert");
        m_SkyboxFragSpv = ShaderCompiler::Compile(shadersPath / "skybox.frag");

        vkDeviceWaitIdle(VulkanContext::Get().GetDevice());
        m_GeoPipelineManager.Clear();
        m_GeoSkinnedPipelineManager.Clear();
        m_ShadowPipeline.reset();
        m_ShadowSkinnedPipeline.reset();
        m_SkyboxPipeline.reset();
        m_BloomExtractPipeline.reset();
        m_BloomBlurPipeline.reset();
        m_PostProcessPipeline.reset();
        m_OutlinePipeline.reset();
        m_GridPipeline.reset();
        m_SelectionMaskPipeline.reset();
        m_SelectionMaskSkinnedPipeline.reset();
        CreatePipelines();

        LH_CORE_INFO("Utility shaders recompiled and pipelines rebuilt");
    }

    // =========================================================================
    // Initialization
    // =========================================================================

    void RenderingSystem::InitGlobalUniforms()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        m_GlobalUniformBuffer = std::make_shared<VKUniformBuffer>(sizeof(GlobalUniforms));

        // Set 0 layout: binding 0 = GlobalUBO, bindings 1-3 = IBL samplers
        VkDescriptorSetLayoutBinding bindings[4] = {};

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

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 4;
        layoutInfo.pBindings = bindings;

        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_GlobalSetLayout);

        VulkanContext::Get().GetDescriptorAllocator().Allocate(m_GlobalSetLayout, m_GlobalDescriptorSet);

        // Write binding 0 (UBO) immediately; bindings 1-3 written after IBL init
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_GlobalUniformBuffer->GetVulkanBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(GlobalUniforms);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_GlobalDescriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    void RenderingSystem::InitShadowResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // --- Shadow map: k_ShadowResolution^2, D32_Float, k_ShadowCascadeCount-layer 2D array (Phase 13) ---
        m_ShadowMap = std::make_shared<VKTexture>(
            k_ShadowResolution, k_ShadowResolution, TextureFormat::D32_Float,
            k_ShadowCascadeCount, /*createFlags*/ 0u, /*mipLevels*/ 1u, /*extraUsage*/ 0u);

        // Per-layer 2D views for ShadowPass.Ci depth attachments (Phase 13C).
        auto shadowTexForViews = std::static_pointer_cast<VKTexture>(m_ShadowMap);
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            m_ShadowLayerViews[i] = shadowTexForViews->CreateLayerView(i);

        // --- Shadow sampler (PCF compare: less) ---
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        samplerInfo.compareEnable = VK_TRUE;
        samplerInfo.compareOp = VK_COMPARE_OP_LESS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        vkCreateSampler(device, &samplerInfo, nullptr, &m_ShadowSampler);

        // --- Light UBO ---
        m_LightUniformBuffer = std::make_shared<VKUniformBuffer>(sizeof(LightUniforms));

        // --- Set 3 descriptor layout: binding 0 = LightUBO, binding 1 = shadow sampler ---
        VkDescriptorSetLayoutBinding bindings[2] = {};

        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo lightLayoutInfo{};
        lightLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lightLayoutInfo.bindingCount = 2;
        lightLayoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &lightLayoutInfo, nullptr, &m_LightSetLayout);

        // --- Pool ---
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = 1;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_LightDescPool);

        // --- Allocate set ---
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_LightDescPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_LightSetLayout;
        vkAllocateDescriptorSets(device, &allocInfo, &m_LightDescSet);

        // --- Write descriptors ---
        VkDescriptorBufferInfo lightBufInfo{};
        lightBufInfo.buffer = m_LightUniformBuffer->GetVulkanBuffer();
        lightBufInfo.offset = 0;
        lightBufInfo.range = sizeof(LightUniforms);

        auto vkShadowTex = std::static_pointer_cast<VKTexture>(m_ShadowMap);
        VkDescriptorImageInfo shadowImgInfo{};
        shadowImgInfo.sampler     = m_ShadowSampler;
        shadowImgInfo.imageView   = vkShadowTex->GetImageView();
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2] = {};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_LightDescSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &lightBufInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_LightDescSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &shadowImgInfo;

        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }

    void RenderingSystem::InitPostProcessResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        u32 w = m_SceneColor->GetWidth();
        u32 h = m_SceneColor->GetHeight();

        // Bloom textures (half-res)
        m_BloomA = Texture::Create(std::max(w / 2, 1u), std::max(h / 2, 1u), TextureFormat::RGBA16F);
        m_BloomB = Texture::Create(std::max(w / 2, 1u), std::max(h / 2, 1u), TextureFormat::RGBA16F);

        // Post-process UBO
        m_PostProcessUBOBuffer = std::make_shared<VKUniformBuffer>(sizeof(PostProcessUBO));

        // Linear clamp sampler
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        vkCreateSampler(device, &samplerInfo, nullptr, &m_PPSampler);

        // Descriptor set layout: [sampler2D, sampler2D, UBO]
        VkDescriptorSetLayoutBinding bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 3;
        layoutInfo.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_PPDescSetLayout);

        // Descriptor pool: 4 sets × (2 samplers + 1 UBO)
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 8; // 4 sets × 2 sampler bindings
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[1].descriptorCount = 4; // 4 sets × 1 UBO binding

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 4;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_PPDescPool);

        // Allocate 4 descriptor sets
        VkDescriptorSetLayout setLayouts[4] = { m_PPDescSetLayout, m_PPDescSetLayout, m_PPDescSetLayout, m_PPDescSetLayout };
        VkDescriptorSet sets[4];
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_PPDescPool;
        allocInfo.descriptorSetCount = 4;
        allocInfo.pSetLayouts = setLayouts;
        vkAllocateDescriptorSets(device, &allocInfo, sets);

        m_BloomExtractDescSet = sets[0];
        m_BloomBlurHDescSet   = sets[1];
        m_BloomBlurVDescSet   = sets[2];
        m_CompositeDescSet    = sets[3];

        UpdatePostProcessDescriptors();

        // ---- Outline pass resources ----
        {
            // Nearest-neighbor sampler for mask and depth textures
            VkSamplerCreateInfo outlineSamplerInfo{};
            outlineSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            outlineSamplerInfo.magFilter = VK_FILTER_NEAREST;
            outlineSamplerInfo.minFilter = VK_FILTER_NEAREST;
            outlineSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &outlineSamplerInfo, nullptr, &m_OutlineSampler);

            // Descriptor set layout: 3 sampler bindings
            VkDescriptorSetLayoutBinding bindings[3] = {};
            // Binding 0: sampler2D (selection mask)
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            // Binding 1: sampler2D (selection depth)
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            // Binding 2: sampler2D (scene depth)
            bindings[2].binding = 2;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo outlineLayoutInfo{};
            outlineLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            outlineLayoutInfo.bindingCount = 3;
            outlineLayoutInfo.pBindings = bindings;
            vkCreateDescriptorSetLayout(device, &outlineLayoutInfo, nullptr, &m_OutlineDescSetLayout);

            // Descriptor pool: 1 set, 3 combined image samplers
            VkDescriptorPoolSize outlinePoolSize{};
            outlinePoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            outlinePoolSize.descriptorCount = 3;

            VkDescriptorPoolCreateInfo outlinePoolInfo{};
            outlinePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            outlinePoolInfo.maxSets = 1;
            outlinePoolInfo.poolSizeCount = 1;
            outlinePoolInfo.pPoolSizes = &outlinePoolSize;
            vkCreateDescriptorPool(device, &outlinePoolInfo, nullptr, &m_OutlineDescPool);

            // Allocate descriptor set
            VkDescriptorSetAllocateInfo outlineAllocInfo{};
            outlineAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            outlineAllocInfo.descriptorPool = m_OutlineDescPool;
            outlineAllocInfo.descriptorSetCount = 1;
            outlineAllocInfo.pSetLayouts = &m_OutlineDescSetLayout;
            vkAllocateDescriptorSets(device, &outlineAllocInfo, &m_OutlineDescSet);

            // Write all 3 descriptors: selection mask, selection depth, scene depth
            auto vkMask      = std::static_pointer_cast<VKTexture>(m_SelectionMask);
            auto vkSelDepth  = std::static_pointer_cast<VKTexture>(m_SelectionDepth);
            auto vkScnDepth  = std::static_pointer_cast<VKTexture>(m_SceneDepth);

            VkDescriptorImageInfo maskImgInfo{};
            maskImgInfo.sampler     = m_OutlineSampler;
            maskImgInfo.imageView   = vkMask->GetImageView();
            maskImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo selDepthImgInfo{};
            selDepthImgInfo.sampler     = m_OutlineSampler;
            selDepthImgInfo.imageView   = vkSelDepth->GetImageView();
            selDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo scnDepthImgInfo{};
            scnDepthImgInfo.sampler     = m_OutlineSampler;
            scnDepthImgInfo.imageView   = vkScnDepth->GetImageView();
            scnDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[3] = {};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = m_OutlineDescSet;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &maskImgInfo;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = m_OutlineDescSet;
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &selDepthImgInfo;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = m_OutlineDescSet;
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &scnDepthImgInfo;

            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }

        // ---- Grid pass resources ----
        {
            // Linear sampler for scene depth read (matching behaviour used elsewhere for depth texture reads)
            VkSamplerCreateInfo gridSamplerInfo{};
            gridSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            gridSamplerInfo.magFilter = VK_FILTER_NEAREST;
            gridSamplerInfo.minFilter = VK_FILTER_NEAREST;
            gridSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            gridSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &gridSamplerInfo, nullptr, &m_GridDepthSampler);

            // Descriptor set layout: binding 0 = GlobalUBO (camera), binding 1 = scene depth sampler
            VkDescriptorSetLayoutBinding gridBindings[2] = {};
            gridBindings[0].binding = 0;
            gridBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            gridBindings[0].descriptorCount = 1;
            gridBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            gridBindings[1].binding = 1;
            gridBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gridBindings[1].descriptorCount = 1;
            gridBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo gridLayoutInfo{};
            gridLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            gridLayoutInfo.bindingCount = 2;
            gridLayoutInfo.pBindings = gridBindings;
            vkCreateDescriptorSetLayout(device, &gridLayoutInfo, nullptr, &m_GridDescSetLayout);

            // Descriptor pool: 1 set (UBO + sampler)
            VkDescriptorPoolSize gridPoolSizes[2] = {};
            gridPoolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            gridPoolSizes[0].descriptorCount = 1;
            gridPoolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gridPoolSizes[1].descriptorCount = 1;

            VkDescriptorPoolCreateInfo gridPoolInfo{};
            gridPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            gridPoolInfo.maxSets = 1;
            gridPoolInfo.poolSizeCount = 2;
            gridPoolInfo.pPoolSizes = gridPoolSizes;
            vkCreateDescriptorPool(device, &gridPoolInfo, nullptr, &m_GridDescPool);

            VkDescriptorSetAllocateInfo gridAllocInfo{};
            gridAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            gridAllocInfo.descriptorPool = m_GridDescPool;
            gridAllocInfo.descriptorSetCount = 1;
            gridAllocInfo.pSetLayouts = &m_GridDescSetLayout;
            vkAllocateDescriptorSets(device, &gridAllocInfo, &m_GridDescSet);

            // Write: global UBO + scene depth
            VkDescriptorBufferInfo gridUBOInfo{};
            gridUBOInfo.buffer = m_GlobalUniformBuffer->GetVulkanBuffer();
            gridUBOInfo.offset = 0;
            gridUBOInfo.range  = sizeof(GlobalUniforms);

            auto vkScnDepthGrid = std::static_pointer_cast<VKTexture>(m_SceneDepth);
            VkDescriptorImageInfo gridDepthImgInfo{};
            gridDepthImgInfo.sampler     = m_GridDepthSampler;
            gridDepthImgInfo.imageView   = vkScnDepthGrid->GetImageView();
            gridDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet gridWrites[2] = {};
            gridWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            gridWrites[0].dstSet = m_GridDescSet;
            gridWrites[0].dstBinding = 0;
            gridWrites[0].descriptorCount = 1;
            gridWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            gridWrites[0].pBufferInfo = &gridUBOInfo;

            gridWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            gridWrites[1].dstSet = m_GridDescSet;
            gridWrites[1].dstBinding = 1;
            gridWrites[1].descriptorCount = 1;
            gridWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            gridWrites[1].pImageInfo = &gridDepthImgInfo;

            vkUpdateDescriptorSets(device, 2, gridWrites, 0, nullptr);
        }
    }

    void RenderingSystem::UpdatePostProcessDescriptors()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        auto sceneVk  = std::static_pointer_cast<VKTexture>(m_SceneColor);
        auto bloomAVk = std::static_pointer_cast<VKTexture>(m_BloomA);
        auto bloomBVk = std::static_pointer_cast<VKTexture>(m_BloomB);

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = m_PostProcessUBOBuffer->GetVulkanBuffer();
        uboInfo.offset = 0;
        uboInfo.range  = sizeof(PostProcessUBO);

        // Helper: write a combined image sampler descriptor
        auto MakeImageInfo = [this](VkImageView view) -> VkDescriptorImageInfo {
            VkDescriptorImageInfo info{};
            info.sampler     = m_PPSampler;
            info.imageView   = view;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return info;
        };

        // BloomExtract: binding 0 = SceneColor, binding 1 = unused (BloomA as placeholder), binding 2 = UBO
        VkDescriptorImageInfo bloomExtractImg0 = MakeImageInfo(sceneVk->GetImageView());
        VkDescriptorImageInfo bloomExtractImg1 = MakeImageInfo(bloomAVk->GetImageView());

        // BloomBlurH: binding 0 = BloomA, binding 1 = unused, binding 2 = UBO
        VkDescriptorImageInfo blurHImg0 = MakeImageInfo(bloomAVk->GetImageView());
        VkDescriptorImageInfo blurHImg1 = MakeImageInfo(bloomBVk->GetImageView());

        // BloomBlurV: binding 0 = BloomB, binding 1 = unused, binding 2 = UBO
        VkDescriptorImageInfo blurVImg0 = MakeImageInfo(bloomBVk->GetImageView());
        VkDescriptorImageInfo blurVImg1 = MakeImageInfo(bloomAVk->GetImageView());

        // Composite: binding 0 = SceneColor (HDR), binding 1 = BloomA (blurred), binding 2 = UBO
        VkDescriptorImageInfo compImg0 = MakeImageInfo(sceneVk->GetImageView());
        VkDescriptorImageInfo compImg1 = MakeImageInfo(bloomAVk->GetImageView());

        // Write all 4 sets (3 writes each = 12 total)
        VkWriteDescriptorSet writes[12] = {};
        int idx = 0;

        auto AddWrite = [&](VkDescriptorSet set, u32 binding, VkDescriptorType type,
                           VkDescriptorImageInfo* imgInfo, VkDescriptorBufferInfo* bufInfo) {
            writes[idx].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[idx].dstSet = set;
            writes[idx].dstBinding = binding;
            writes[idx].descriptorCount = 1;
            writes[idx].descriptorType = type;
            writes[idx].pImageInfo = imgInfo;
            writes[idx].pBufferInfo = bufInfo;
            idx++;
        };

        // BloomExtract set
        AddWrite(m_BloomExtractDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bloomExtractImg0, nullptr);
        AddWrite(m_BloomExtractDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bloomExtractImg1, nullptr);
        AddWrite(m_BloomExtractDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // BloomBlurH set
        AddWrite(m_BloomBlurHDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurHImg0, nullptr);
        AddWrite(m_BloomBlurHDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurHImg1, nullptr);
        AddWrite(m_BloomBlurHDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // BloomBlurV set
        AddWrite(m_BloomBlurVDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurVImg0, nullptr);
        AddWrite(m_BloomBlurVDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurVImg1, nullptr);
        AddWrite(m_BloomBlurVDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        // Composite set
        AddWrite(m_CompositeDescSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &compImg0, nullptr);
        AddWrite(m_CompositeDescSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &compImg1, nullptr);
        AddWrite(m_CompositeDescSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uboInfo);

        vkUpdateDescriptorSets(device, idx, writes, 0, nullptr);
    }

    void RenderingSystem::UpdatePostProcessUBO()
    {
        PostProcessUBO ubo{};
        ubo.bloomThreshold      = m_PostProcessSettings.bloomThreshold;
        ubo.bloomStrength       = m_PostProcessSettings.bloomStrength;
        ubo.exposure            = m_PostProcessSettings.exposure;
        ubo.contrast            = m_PostProcessSettings.contrast;
        ubo.saturation          = m_PostProcessSettings.saturation;
        ubo.tonemapOp           = static_cast<int>(m_PostProcessSettings.tonemapOp);
        ubo.vignetteAmount      = m_PostProcessSettings.vignetteAmount;
        ubo.vignetteHardness    = m_PostProcessSettings.vignetteHardness;
        ubo.grainAmount         = m_PostProcessSettings.grainAmount;
        ubo.sharpness           = m_PostProcessSettings.sharpness;
        ubo.chromaticAberration = m_PostProcessSettings.chromaticAberration;
        ubo.time                = Time::GetTime();
        ubo.shadowBalance       = m_PostProcessSettings.shadowBalance;
        ubo.midtoneBalance      = m_PostProcessSettings.midtoneBalance;
        ubo.highlightBalance    = m_PostProcessSettings.highlightBalance;
        m_PostProcessUBOBuffer->SetData(&ubo, sizeof(PostProcessUBO));
    }

    // =========================================================================
    // IBL precomputation
    // =========================================================================

    void RenderingSystem::InitIBLResources(const fs::path& hdrPath)
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Run precomputation (equirect -> cubemap -> irradiance -> prefilter -> BRDF LUT)
        IBLResult ibl = IBL::Precompute(hdrPath);

        m_IrradianceMap  = ibl.irradianceMap;
        m_PrefilteredMap = ibl.prefilteredMap;
        m_BRDFLut        = ibl.brdfLut;
        m_IBLSampler     = ibl.iblSampler;
        m_SkyboxVB       = ibl.skyboxVB;
        m_SkyboxVertSpv  = std::move(ibl.skyboxVertSpv);
        m_SkyboxFragSpv  = std::move(ibl.skyboxFragSpv);

        // Write IBL descriptors to Set 0 (bindings 1-3)
        {
            auto vkIrr = std::static_pointer_cast<VKTexture>(m_IrradianceMap);
            auto vkPf  = std::static_pointer_cast<VKTexture>(m_PrefilteredMap);
            auto vkLut = std::static_pointer_cast<VKTexture>(m_BRDFLut);

            VkDescriptorImageInfo irrInfo{};
            irrInfo.sampler = m_IBLSampler;
            irrInfo.imageView = vkIrr->GetImageView();
            irrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo pfInfo{};
            pfInfo.sampler = m_IBLSampler;
            pfInfo.imageView = vkPf->GetImageView();
            pfInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo lutInfo{};
            lutInfo.sampler = m_IBLSampler;
            lutInfo.imageView = vkLut->GetImageView();
            lutInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet writes[3] = {};
            writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[0].dstSet = m_GlobalDescriptorSet;
            writes[0].dstBinding = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo = &irrInfo;

            writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[1].dstSet = m_GlobalDescriptorSet;
            writes[1].dstBinding = 2;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &pfInfo;

            writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            writes[2].dstSet = m_GlobalDescriptorSet;
            writes[2].dstBinding = 3;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].descriptorCount = 1;
            writes[2].pImageInfo = &lutInfo;

            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }
    }

    void RenderingSystem::ReloadSkybox(const fs::path& hdrPath)
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        vkDeviceWaitIdle(device);

        // Destroy old IBL sampler (textures freed by shared_ptr reset in InitIBLResources)
        if (m_IBLSampler) {
            vkDestroySampler(device, m_IBLSampler, nullptr);
            m_IBLSampler = VK_NULL_HANDLE;
        }

        InitIBLResources(hdrPath);

        // Rebuild skybox pipeline (new prefiltered map may have different mip count)
        m_SkyboxPipeline.reset();
        CreatePipelines();

        LH_CORE_INFO("Skybox reloaded from '{}'", hdrPath.string());
    }

    // =========================================================================
    // Pipeline creation
    // =========================================================================

    void RenderingSystem::InitObjectSSBODescriptorLayout()
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

    void RenderingSystem::CreatePipelines()
    {
        // Ensure Set 5 descriptor layout exists (idempotent — safe to call on hot-reload)
        InitObjectSSBODescriptorLayout();

        // Full vertex layout (matches Model::ProcessMeshData)
        BufferLayout vertexLayout = {
            { ShaderDataType::Float3, "a_Position"  },
            { ShaderDataType::Float3, "a_Normal"    },
            { ShaderDataType::Float2, "a_TexCoord0" },
            { ShaderDataType::Float2, "a_TexCoord1" },
            { ShaderDataType::Float3, "a_Tangent"   }
        };

        auto bindingDescs = vertexLayout.GetBindingDescriptions();
        auto attribDescs  = vertexLayout.GetAttributeDescriptions();

        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(ObjectPushConstants);

        // 5-set layout for shadow / selection-mask / skybox pipelines (push constants remain)
        std::vector<VkDescriptorSetLayout> layouts = {
            m_GlobalSetLayout,                                    // Set 0
            VulkanContext::Get().GetBindlessSet().GetLayout(),   // Set 1
            MaterialSystem::GetDescriptorSetLayout(),            // Set 2
            m_LightSetLayout,                                    // Set 3
            BoneMatrixBuffer::GetDescriptorSetLayout()           // Set 4
        };

        // 6-set layout for geometry pipelines (adds Set 5 = GPUObjectData SSBO, no push constants)
        std::vector<VkDescriptorSetLayout> geoLayouts = {
            m_GlobalSetLayout,                                    // Set 0
            VulkanContext::Get().GetBindlessSet().GetLayout(),   // Set 1
            MaterialSystem::GetDescriptorSetLayout(),            // Set 2
            m_LightSetLayout,                                    // Set 3
            BoneMatrixBuffer::GetDescriptorSetLayout(),          // Set 4
            m_ObjectSSBODescLayout                               // Set 5
        };

        // ---- PBR geometry pipeline manager (lazy creation keyed by {shaderUUID, renderMode}) ----
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

        // ---- Shadow pipeline (depth-only, position attribute only) ----
        // Same stride as full vertex, but only declare the position attribute.
        BufferLayout shadowVertexLayout = {
            { ShaderDataType::Float3, "a_Position" }
        };
        // Override stride to match the full vertex size so the buffer reads are correct.
        auto shadowBindingDescs = shadowVertexLayout.GetBindingDescriptions();
        auto shadowAttribDescs  = shadowVertexLayout.GetAttributeDescriptions();
        // Fix stride: the real vertex buffer has stride = sizeof(Vertex) = 52 bytes.
        // BufferLayout calculates stride from declared attributes only.
        // We need to manually set the stride to 52 to match the actual buffer.
        if (!shadowBindingDescs.empty())
            shadowBindingDescs[0].stride = sizeof(float) * (3 + 3 + 2 + 2 + 3); // 52 bytes

        // 4-byte VERTEX push constant carries cascadeIndex (Phase 13C).
        VkPushConstantRange shadowCascadePC{};
        shadowCascadePC.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        shadowCascadePC.offset     = 0;
        shadowCascadePC.size       = sizeof(u32);

        PipelineConfig shadowConfig;
        shadowConfig.colorFormats = {};  // depth-only
        shadowConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
        shadowConfig.depthTest = true; shadowConfig.depthWrite = true;
        shadowConfig.blendEnabled = false;
        shadowConfig.cullMode = VK_CULL_MODE_FRONT_BIT; // front-face culling reduces shadow acne
        shadowConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        shadowConfig.bindingDescriptions = shadowBindingDescs;
        shadowConfig.attributeDescriptions = shadowAttribDescs;
        shadowConfig.pushConstantRanges = { shadowCascadePC };

        m_ShadowPipeline = std::make_unique<VKPipeline>(shadowConfig, m_ShadowVertSpv, m_ShadowFragSpv, geoLayouts);

        // ---- Skinned geometry pipeline manager ----
        BufferLayout skinnedVertexLayout = {
            { ShaderDataType::Float3, "a_Position"    },
            { ShaderDataType::Float3, "a_Normal"      },
            { ShaderDataType::Float2, "a_TexCoord0"   },
            { ShaderDataType::Float2, "a_TexCoord1"   },
            { ShaderDataType::Float3, "a_Tangent"     },
            { ShaderDataType::Int4,   "a_BoneIDs"     },
            { ShaderDataType::Float4, "a_BoneWeights" }
        };

        auto skinnedBindingDescs = skinnedVertexLayout.GetBindingDescriptions();
        auto skinnedAttribDescs  = skinnedVertexLayout.GetAttributeDescriptions();

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

        // ---- Skinned shadow pipeline (depth-only, full skinned vertex stride) ----
        if (!m_ShadowSkinnedVertSpv.empty())
        {
            // Full skinned vertex layout — all 7 attributes declared so stride = 84 bytes
            PipelineConfig shadowSkinnedConfig;
            shadowSkinnedConfig.colorFormats = {};
            shadowSkinnedConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
            shadowSkinnedConfig.depthTest = true; shadowSkinnedConfig.depthWrite = true;
            shadowSkinnedConfig.blendEnabled = false;
            shadowSkinnedConfig.cullMode = VK_CULL_MODE_FRONT_BIT;
            shadowSkinnedConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            shadowSkinnedConfig.bindingDescriptions = skinnedBindingDescs;
            shadowSkinnedConfig.attributeDescriptions = skinnedAttribDescs;
            shadowSkinnedConfig.pushConstantRanges = { shadowCascadePC };

            m_ShadowSkinnedPipeline = std::make_unique<VKPipeline>(
                shadowSkinnedConfig, m_ShadowSkinnedVertSpv, m_ShadowFragSpv, geoLayouts);
        }

        // ---- Selection mask pipeline (static) ----
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

        // ---- Selection mask pipeline (skinned) ----
        if (!m_SelectionMaskSkinnedVertSpv.empty() && !m_SelectionMaskFragSpv.empty())
        {
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

        // ---- Skybox pipeline ----
        if (!m_SkyboxVertSpv.empty() && !m_SkyboxFragSpv.empty())
        {
            BufferLayout skyboxVertexLayout = {
                { ShaderDataType::Float3, "a_Position" }
            };

            PipelineConfig skyboxConfig;
            skyboxConfig.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT };
            skyboxConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
            skyboxConfig.depthTest = true;
            skyboxConfig.depthWrite = false;
            skyboxConfig.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            skyboxConfig.blendEnabled = false;
            skyboxConfig.cullMode = VK_CULL_MODE_BACK_BIT; // Y-flipped projection reverses winding; cull back = show inside faces
            skyboxConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            skyboxConfig.bindingDescriptions = skyboxVertexLayout.GetBindingDescriptions();
            skyboxConfig.attributeDescriptions = skyboxVertexLayout.GetAttributeDescriptions();

            m_SkyboxPipeline = std::make_unique<VKPipeline>(skyboxConfig, m_SkyboxVertSpv, m_SkyboxFragSpv, layouts);
        }

        // ---- Post-process pipelines ----
        if (!m_FullscreenVertSpv.empty() && m_PPDescSetLayout != VK_NULL_HANDLE)
        {
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

        // ---- Outline pipeline ----
        if (!m_FullscreenVertSpv.empty() && !m_OutlineFragSpv.empty() && m_OutlineDescSetLayout != VK_NULL_HANDLE)
        {
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

        // ---- Grid pipeline (editor-only infinite grid fullscreen pass) ----
        if (!m_FullscreenVertSpv.empty() && !m_GridFragSpv.empty() && m_GridDescSetLayout != VK_NULL_HANDLE)
        {
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

    u32 RenderingSystem::EnsureMaterialRegistered(std::shared_ptr<Material> material)
    {
        auto it = m_MaterialSlotMap.find(material->Handle);
        if (it != m_MaterialSlotMap.end())
            return it->second;

        u32 slot = MaterialSystem::RegisterMaterial(material);
        m_MaterialSlotMap[material->Handle] = slot;
        return slot;
    }

    // =========================================================================
    // Per-frame updates
    // =========================================================================

    void RenderingSystem::ComputeCascadeSplits(float nearZ, float farZ, float lambda,
                                                float outFar[k_ShadowCascadeCount]) const
    {
        // Engel "Practical Split": lambda * Clog + (1-lambda) * Cuniform.
        const float ratio = farZ / std::max(nearZ, 1e-4f);
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
        {
            float p    = float(i + 1) / float(k_ShadowCascadeCount);
            float clog = nearZ * std::pow(ratio, p);
            float cuni = nearZ + (farZ - nearZ) * p;
            outFar[i]  = lambda * clog + (1.0f - lambda) * cuni;
        }
    }

    glm::mat4 RenderingSystem::ComputeCascadeMatrix(float nearD, float farD,
                                                     const glm::vec3& lightDir,
                                                     float tanHalfFovY, float aspect,
                                                     const glm::mat4& camViewInv,
                                                     bool stabilize) const
    {
        // 8 corners of the sub-frustum slice [nearD, farD] in view space, then world space.
        const float hN = nearD * tanHalfFovY;
        const float wN = hN * aspect;
        const float hF = farD * tanHalfFovY;
        const float wF = hF * aspect;

        const glm::vec4 cornersVS[8] = {
            { -wN, -hN, -nearD, 1.0f }, {  wN, -hN, -nearD, 1.0f },
            {  wN,  hN, -nearD, 1.0f }, { -wN,  hN, -nearD, 1.0f },
            { -wF, -hF, -farD,  1.0f }, {  wF, -hF, -farD,  1.0f },
            {  wF,  hF, -farD,  1.0f }, { -wF,  hF, -farD,  1.0f },
        };

        glm::vec3 cornersWS[8];
        glm::vec3 center(0.0f);
        for (int i = 0; i < 8; ++i) {
            glm::vec4 w = camViewInv * cornersVS[i];
            cornersWS[i] = glm::vec3(w) / w.w;
            center += cornersWS[i];
        }
        center *= (1.0f / 8.0f);

        const glm::vec3 up = (glm::abs(glm::dot(lightDir, glm::vec3(0, 1, 0))) > 0.99f)
                             ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);

        // Extra depth range behind the light so off-frustum casters still cast shadows.
        constexpr float kCasterExtend = 50.0f;

        if (stabilize) {
            // Bounding sphere — rotation-invariant → no shimmer when camera rotates.
            float radius = 0.0f;
            for (int i = 0; i < 8; ++i)
                radius = glm::max(radius, glm::length(cornersWS[i] - center));
            radius = std::ceil(radius * 16.0f) / 16.0f; // quantize so minor motion doesn't resize

            float ext = glm::max(radius, 0.01f);

            glm::mat4 lightView = glm::lookAt(center - lightDir * ext, center, up);

            // Texel snap the origin in light space to kill sub-pixel shimmer.
            const float texelSize = (2.0f * ext) / float(k_ShadowResolution);
            glm::vec4 originLS = lightView * glm::vec4(center, 1.0f);
            originLS.x = std::floor(originLS.x / texelSize) * texelSize;
            originLS.y = std::floor(originLS.y / texelSize) * texelSize;
            const glm::vec4 originWS = glm::inverse(lightView) * originLS;
            const glm::vec3 snappedCenter = glm::vec3(originWS);
            lightView = glm::lookAt(snappedCenter - lightDir * ext, snappedCenter, up);

            // Negative near plane pulls the slab back behind the virtual light eye so
            // casters above/behind the frustum slice (tall geometry, buildings) still
            // get rendered. Without this, small cascades (near camera) drop shadows
            // because ext alone isn't enough depth range toward the sun.
            glm::mat4 lightProj = glm::ortho(-ext, ext, -ext, ext,
                                              -kCasterExtend, 2.0f * ext + kCasterExtend);
            lightProj[1][1] *= -1.0f; // Vulkan Y-flip
            return lightProj * lightView;
        }
        else {
            // Tight AABB in light-view space.
            glm::mat4 lightView = glm::lookAt(center - lightDir, center, up);

            glm::vec3 mn( FLT_MAX);
            glm::vec3 mx(-FLT_MAX);
            for (int i = 0; i < 8; ++i) {
                glm::vec4 ls = lightView * glm::vec4(cornersWS[i], 1.0f);
                mn = glm::min(mn, glm::vec3(ls));
                mx = glm::max(mx, glm::vec3(ls));
            }

            // Clamp degenerate extents.
            const float extX = glm::max(mx.x - mn.x, 0.01f);
            const float extY = glm::max(mx.y - mn.y, 0.01f);
            mx.x = mn.x + extX;
            mx.y = mn.y + extY;

            // lookAt(center - lightDir, ...) puts the light "behind" the slab;
            // in right-handed light space the slab lies at negative Z. Extend near
            // plane outward to pick up casters behind the frustum.
            glm::mat4 lightProj = glm::ortho(mn.x, mx.x, mn.y, mx.y,
                                              -mx.z - kCasterExtend, -mn.z);
            lightProj[1][1] *= -1.0f; // Vulkan Y-flip
            return lightProj * lightView;
        }
    }

    void RenderingSystem::UpdateLightUniforms(Scene* scene)
    {
        auto& registry = scene->Registry();
        LightUniforms lights{};

        // Directional light — use first entity found
        bool foundDir = false;
        float splitLambda      = 0.5f;
        float shadowDistance   = 200.0f;
        bool  stabilize        = true;
        auto dirView = registry.view<WorldTransform, DirectionalLight>();
        for (auto [entity, wt, dl] : dirView.each())
        {
            if (!foundDir)
            {
                // Forward vector is -Z column of world matrix
                lights.dirLight.direction = glm::normalize(-glm::vec3(wt.Matrix[2]));
                lights.dirLight.color     = dl.Color;
                lights.dirLight.intensity = dl.Intensity;
                m_CachedCastShadows  = dl.CastShadows;
                m_CachedShadowBias   = glm::vec4(dl.ShadowBias[0], dl.ShadowBias[1], dl.ShadowBias[2], dl.ShadowBias[3]);
                m_CachedShadowNormalBias = glm::vec4(dl.ShadowNormalBias[0], dl.ShadowNormalBias[1], dl.ShadowNormalBias[2], dl.ShadowNormalBias[3]);
                splitLambda    = glm::clamp(dl.SplitLambda, 0.0f, 1.0f);
                shadowDistance = dl.ShadowDistance;
                stabilize      = dl.StabilizeCascades;
                foundDir = true;
            }
        }

        if (!foundDir)
        {
            lights.dirLight.direction = glm::normalize(glm::vec3(1.0f, 1.0f, 0.5f));
            lights.dirLight.color     = glm::vec3(1.0f);
            lights.dirLight.intensity = 3.0f;
        }

        // Point lights
        int count = 0;
        auto pointView = registry.view<WorldTransform, PointLight>();
        for (auto [entity, wt, pl] : pointView.each())
        {
            if (count >= 64) break;
            lights.pointLights[count].position  = glm::vec3(wt.Matrix[3]);
            lights.pointLights[count].color     = pl.Color;
            lights.pointLights[count].intensity = pl.Intensity;
            lights.pointLights[count].range     = pl.Range;
            ++count;
        }
        lights.numPointLights = count;

        m_LightUniformBuffer->SetData(&lights, sizeof(LightUniforms));

        // ── PSSM split computation + per-cascade ortho fitting (Phase 13B) ──
        const glm::vec3 lightDir = lights.dirLight.direction;

        // FOV / aspect from unflipped perspective projection.
        // projection[1][1] = 1/tan(fovY/2); projection[0][0] = 1/(aspect*tan(fovY/2)).
        const glm::mat4& proj = m_CameraParams.projection;
        const float tanHalfFovY = (proj[1][1] != 0.0f) ? (1.0f / proj[1][1]) : 1.0f;
        const float aspect      = (proj[0][0] != 0.0f) ? (proj[1][1] / proj[0][0]) : 1.0f;
        const glm::mat4 camViewInv = glm::inverse(m_CameraParams.view);

        const float nearZ = glm::max(m_CameraParams.nearZ, 1e-3f);
        const float farZ  = glm::max(nearZ + 1e-3f,
                                     glm::min(m_CameraParams.farZ, shadowDistance));

        float cascadeFar[k_ShadowCascadeCount];
        ComputeCascadeSplits(nearZ, farZ, splitLambda, cascadeFar);

        float cascadeNear = nearZ;
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
        {
            const float cf = cascadeFar[i];
            m_CachedLightSpaceMatrix[i] = ComputeCascadeMatrix(
                cascadeNear, cf, lightDir, tanHalfFovY, aspect, camViewInv, stabilize);
            cascadeNear = cf;
        }

        // GLSL-side cascade selection uses absolute view-Z distances (positive).
        m_CachedCascadeSplitsViewZ = glm::vec4(cascadeFar[0], cascadeFar[1], cascadeFar[2], cascadeFar[3]);
    }

    // =========================================================================
    // GPU Object Buffer + Cull Pipeline
    // =========================================================================

    void RenderingSystem::InitGPUObjectBuffers()
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
            k_MaxGPUObjects * sizeof(GPUObjectData),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            m_ObjectSSBO, m_ObjectSSBOAlloc, m_ObjectSSBOMapped);

        // Indirect buffer holds 5 regions (camera + 4 cascades), each with k_IndirectRegionStride commands.
        allocBuffer(
            k_IndirectRegionCount * k_IndirectRegionStride * sizeof(VkDrawIndexedIndirectCommand),
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

    void RenderingSystem::InitCullPipeline()
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
        pcRange.size       = sizeof(glm::vec4) * 6 + sizeof(u32) * 2;

        auto spv = ShaderCompiler::Compile(FileSystem::EngineAssetsPath("shaders/gpu_cull.comp"));
        if (spv.empty())
        {
            LH_CORE_ERROR("RenderingSystem: Failed to compile gpu_cull.comp!");
            return;
        }

        m_CullPipeline = std::make_unique<VKComputePipeline>(
            spv,
            std::vector<VkDescriptorSetLayout>{ m_CullDescLayout },
            std::vector<VkPushConstantRange>{ pcRange });
    }

    void RenderingSystem::BuildGPUObjectBuffer(entt::registry& registry)
    {
        auto* objectData   = static_cast<GPUObjectData*>(m_ObjectSSBOMapped);
        auto* indirectCmds = static_cast<VkDrawIndexedIndirectCommand*>(m_IndirectBufferMapped);
        u32   count        = 0;

        // Rebuild entity lookup table here (consumed by GeometryPass + mouse picking)
        // index 0 = null sentinel; valid entities start at index 1
        m_EntityLookup.clear();
        m_EntityLookup.push_back(entt::null);
        m_EntityToSSBOIndex.clear();

        auto view = registry.view<WorldTransform, MeshRenderer>();
        for (auto [entity, wt, mr] : view.each())
        {
            if (count >= k_MaxGPUObjects) break;
            if (!mr.ModelUUID.IsValid()) continue;

            auto model = AssetManager::GetAsset<Model>(mr.ModelUUID);
            if (!model) continue;
            const auto& meshesData = model->GetMeshesData();
            if (mr.MeshIndex >= (u32)meshesData.size()) continue;
            auto mesh = model->GetMesh(mr.MeshIndex);
            if (!mesh) continue;

            GPUObjectData& obj = objectData[count];
            obj.model = wt.Matrix;

            // Bounding sphere from BindPoseAABB (local space)
            const auto& aabb   = meshesData[mr.MeshIndex].BindPoseAABB;
            obj.boundingSphere = glm::vec4(aabb.Center(), glm::length(aabb.Extents()));

            // Material slot
            u32 matSlot = 0;
            if (mr.MaterialUUID.IsValid()) {
                auto it = m_MaterialSlotMap.find(mr.MaterialUUID);
                if (it != m_MaterialSlotMap.end()) matSlot = it->second;
            }
            obj.materialIndex = matSlot;
            obj.shadeMode     = static_cast<u32>(m_ShadeMode);
            // entityID is 1-indexed so the fragment shader output matches m_EntityLookup
            obj.entityID      = (u32)m_EntityLookup.size();  // assigned before push_back
            obj.boneOffset    = 0;

            m_EntityLookup.push_back(entity);      // m_EntityLookup[count + 1] = entity
            m_EntityToSSBOIndex[entity] = count;   // entity → 0-based SSBO index

            // Skinned mesh: get bone offset from Animation component on this entity
            if (meshesData[mr.MeshIndex].IsSkinned && registry.all_of<Animation>(entity))
            {
                auto& anim = registry.get<Animation>(entity);
                if (anim.BufferAllocated)
                    obj.boneOffset = anim.BoneBufferOffset;
            }

            auto* ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer()).get();
            obj.indexCount   = ib ? ib->GetCount() : 0;
            obj.firstIndex   = 0;
            obj.vertexOffset = 0;
            obj._pad         = 0;

            // Indirect command — instanceCount=1; per-region GPU cull zeros it if culled.
            // firstInstance = SSBO index (gl_BaseInstance in shader → objects[gl_BaseInstance]).
            // Duplicate into all k_IndirectRegionCount regions (camera + 4 cascades) so each
            // region has its own independently-cullable command for this object.
            VkDrawIndexedIndirectCommand baseCmd{};
            baseCmd.indexCount    = obj.indexCount;
            baseCmd.instanceCount = 1;
            baseCmd.firstIndex    = 0;
            baseCmd.vertexOffset  = 0;
            baseCmd.firstInstance = count;
            for (u32 r = 0; r < k_IndirectRegionCount; ++r)
                indirectCmds[r * k_IndirectRegionStride + count] = baseCmd;
            count++;
        }

        m_GPUObjectCount = count;
    }

    void RenderingSystem::UpdateGlobalUniforms()
    {
        GlobalUniforms ubo{};
        ubo.view = m_CameraParams.view;
        ubo.projection = m_CameraParams.projection;
        ubo.projection[1][1] *= -1.0f;  // Vulkan Y-flip (shader only, not ImGuizmo)
        ubo.viewProjection = ubo.projection * ubo.view;
        ubo.cameraPos = m_CameraParams.position;
        ubo.time = Time::GetTime();
        for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
            ubo.lightSpaceMatrix[i] = m_CachedLightSpaceMatrix[i];
        ubo.cascadeSplitsViewZ = m_CachedCascadeSplitsViewZ;
        // Negative bias (sentinel) disables shadows entirely in the PBR shader.
        ubo.shadowBias       = m_CachedCastShadows ? m_CachedShadowBias : glm::vec4(-1.0f);
        ubo.shadowNormalBias = m_CachedShadowNormalBias;
        ubo.iblIntensity    = m_CameraParams.iblIntensity;
        ubo.skyboxIntensity = m_CameraParams.skyboxIntensity;
        ubo.debugVisualizeCascades = 0.0f;      // enabled in 13F

        m_GlobalUniformBuffer->SetData(&ubo, sizeof(GlobalUniforms));
        m_CachedViewProj = ubo.viewProjection;
    }

    // =========================================================================
    // Main Update
    // =========================================================================

    void RenderingSystem::Update(Scene* scene)
    {
        LH_PROFILE_FUNCTION();
        auto& registry = scene->Registry();

        // Drain pending shader reloads (queued by FileWatcher on background thread)
        {
            std::lock_guard lock(m_ReloadMutex);
            for (const auto& name : m_PendingReloads)
            {
                LH_CORE_INFO("Shader file changed — reloading '{}'", name);
                ShaderLibrary::Reload(name);
            }
            m_PendingReloads.clear();

            if (m_PendingUtilityReload)
            {
                LH_CORE_INFO("Utility shader changed — recompiling all utility shaders");
                RecompileUtilityShaders();
                m_PendingUtilityReload = false;
            }
        }

        m_FrameAllocator->Reset();

        // --- Frame Debugger: Frozen state → re-render captured frame ---
        if (m_FrameDebugger.state == DebuggerState::Frozen)
        {
            if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan)
            {
                UpdateGlobalUniforms(); // camera may have moved
                RenderCapturedFrame(m_FrameDebugger.drawLimit, scene);
            }
            return;
        }

        // --- Frame Debugger: Prepare for capture ---
        if (m_FrameDebugger.state == DebuggerState::CaptureRequested)
            m_FrameDebugger.capturedFrame.Clear();

        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan)
        {
            // Light uniforms first (sets m_CachedLightSpaceMatrix)
            UpdateLightUniforms(scene);
            // Global UBO second (reads m_CachedLightSpaceMatrix)
            UpdateGlobalUniforms();

            // Register materials and hold assets for all visible entities
            auto matView = registry.view<WorldTransform, MeshRenderer>();
            for (auto [entity, wt, mr] : matView.each())
            {
                if (mr.ModelUUID.IsValid())
                {
                    auto model = AssetManager::GetAsset<Model>(mr.ModelUUID);
                    if (model)
                        scene->HoldAsset(mr.ModelUUID, model);
                }
                if (!mr.MaterialUUID.IsValid()) continue;
                auto material = AssetManager::GetAsset<Material>(mr.MaterialUUID);
                if (material)
                {
                    scene->HoldAsset(mr.MaterialUUID, material);
                    EnsureMaterialRegistered(material);
                }
            }

            // Upload dirty materials
            MaterialSystem::Update(VK_NULL_HANDLE);

            // Upload post-process settings
            UpdatePostProcessUBO();

            // Build GPU object buffer (after materials are registered)
            BuildGPUObjectBuffer(registry);

            RG::RenderGraph rg(*m_FrameAllocator);

            // Import persistent buffers into render graph for barrier tracking
            RG::BufferDesc objDesc {
                "ObjectSSBO",
                k_MaxGPUObjects * sizeof(GPUObjectData),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            };
            RG::BufferDesc indDesc {
                "IndirectBuffer",
                k_IndirectRegionCount * k_IndirectRegionStride * sizeof(VkDrawIndexedIndirectCommand),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
            };
            RG::BufferHandle hObjectBuf   = rg.ImportBuffer(objDesc,   (void*)m_ObjectSSBO,    RG::ResourceState::Undefined);
            RG::BufferHandle hIndirectBuf = rg.ImportBuffer(indDesc,    (void*)m_IndirectBuffer, RG::ResourceState::Undefined);

            // Frustum cull — 5 dispatches: camera region + 4 shadow cascade regions.
            // Each cascade uses its own light-space viewProj frustum so shadow casters
            // outside the camera frustum but inside the cascade still get rendered.
            {
                Frustum camFrustum = CreateFrustumFromCamera(m_CachedViewProj);
                AddCullComputePass(rg, hObjectBuf, hIndirectBuf,
                    m_CullPipeline.get(), m_CullDescSet, camFrustum.planes, m_GPUObjectCount,
                    /*destOffset*/ 0, "FrustumCull.Cam", &m_FrameDebugger);

                for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                {
                    Frustum cascadeFrustum = CreateFrustumFromCamera(m_CachedLightSpaceMatrix[i]);
                    const u32 destOffset = (i + 1) * k_IndirectRegionStride;
                    const std::string name = "FrustumCull.C" + std::to_string(i);
                    AddCullComputePass(rg, hObjectBuf, hIndirectBuf,
                        m_CullPipeline.get(), m_CullDescSet, cascadeFrustum.planes, m_GPUObjectCount,
                        destOffset, name.c_str(), &m_FrameDebugger);
                }
            }

            RG::ResourceHandle shadowHandles[k_ShadowCascadeCount];
            for (u32 i = 0; i < k_ShadowCascadeCount; ++i)
                shadowHandles[i] = AddShadowPass(rg, registry, hIndirectBuf, i);
            auto geoOutput                 = AddGeometryPass(rg, registry, shadowHandles, hIndirectBuf);
            auto maskOutput                = AddSelectionMaskPass(rg, registry);
            RG::ResourceHandle skyboxColor = AddSkyboxPass(rg, geoOutput.color, geoOutput.depth);
            RG::ResourceHandle bloomResult = AddBloomPasses(rg, skyboxColor); // bloom reads PRE-grid color so grid lines don't bloom
            RG::ResourceHandle gridColor   = m_GridVisible
                                             ? AddGridPass(rg, skyboxColor, geoOutput.depth)
                                             : skyboxColor;
            RG::ResourceHandle ldrOutput   = AddPostProcessPass(rg, gridColor, bloomResult);
            RG::ResourceHandle finalOutput = AddOutlinePass(rg, ldrOutput, maskOutput, geoOutput.depth);
            AddImGuiPass(rg, finalOutput);

            rg.Compile();

            // Capture render graph snapshot for Frame Debugger panel
            m_GraphSnapshot = CaptureSnapshot(rg);

            // Read GPU timing from completed frames and fill snapshot
            std::vector<float> gpuTimes;
            u32 nonCulledCount = 0;
            for (auto& p : m_GraphSnapshot.passes)
                if (!p.culled) nonCulledCount++;

            m_GPUTimers.ReadResults(nonCulledCount, gpuTimes);
            float totalMs = 0.0f;
            u32 timerIdx = 0;
            for (auto& p : m_GraphSnapshot.passes)
            {
                if (p.culled) continue;
                if (timerIdx < (u32)gpuTimes.size())
                {
                    p.gpuTimeMs = gpuTimes[timerIdx];
                    if (gpuTimes[timerIdx] > 0.0f) totalMs += gpuTimes[timerIdx];
                }
                timerIdx++;
            }
            m_GraphSnapshot.totalGpuTimeMs = totalMs;

            Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex(), &m_GPUTimers);

            // --- Frame Debugger: Finalize capture and enter frozen state ---
            if (m_FrameDebugger.state == DebuggerState::CaptureRequested)
            {
                // Copy draw command vectors for re-recording
                m_FrameDebugger.capturedOpaqueDraws      = m_OpaqueDraws;
                m_FrameDebugger.capturedCutoutDraws      = m_CutoutDraws;
                m_FrameDebugger.capturedTransparentDraws = m_TransparentDraws;

                // Copy resource and timing info from the graph snapshot
                m_FrameDebugger.capturedFrame.resources      = m_GraphSnapshot.resources;
                m_FrameDebugger.capturedFrame.totalGpuTimeMs = m_GraphSnapshot.totalGpuTimeMs;

                // Copy per-pass GPU times into captured passes
                {
                    u32 capturedIdx = 0;
                    for (auto& ps : m_GraphSnapshot.passes)
                    {
                        if (ps.culled) continue;
                        if (capturedIdx < m_FrameDebugger.capturedFrame.passes.size())
                            m_FrameDebugger.capturedFrame.passes[capturedIdx].gpuTimeMs = ps.gpuTimeMs;
                        capturedIdx++;
                    }
                }

                m_FrameDebugger.capturedFrame.valid = true;
                m_FrameDebugger.state       = DebuggerState::Frozen;
                m_FrameDebugger.drawLimit   = (u32)m_FrameDebugger.capturedFrame.drawCalls.size();
            }

            // --- Mouse picking readback (immediate, single pixel) ---
            if (m_PickPending)
            {
                m_PickPending = false;
                int px = m_PickCoord.x;
                int py = m_PickCoord.y;

                if (px >= 0 && py >= 0 && px < (int)m_EntityIDBuffer->GetWidth() && py < (int)m_EntityIDBuffer->GetHeight())
                {
                    auto vkID = std::static_pointer_cast<VKTexture>(m_EntityIDBuffer);

                    // Create a small staging buffer for readback
                    VkBuffer stagingBuf;
                    VkBufferCreateInfo bufInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    bufInfo.size = sizeof(u32);
                    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                    VmaAllocation stagingAlloc = VulkanAllocator::AllocateBuffer(bufInfo, VMA_MEMORY_USAGE_GPU_TO_CPU, stagingBuf);

                    VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd)
                    {
                        // Transition entity ID image to transfer src
                        VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
                        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                        barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
                        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        barrier.image = vkID->GetImage();
                        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

                        VkDependencyInfo dep{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                        dep.imageMemoryBarrierCount = 1;
                        dep.pImageMemoryBarriers = &barrier;
                        vkCmdPipelineBarrier2(cmd, &dep);

                        // Copy single pixel
                        VkBufferImageCopy region{};
                        region.bufferOffset = 0;
                        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                        region.imageOffset = { px, py, 0 };
                        region.imageExtent = { 1, 1, 1 };
                        vkCmdCopyImageToBuffer(cmd, vkID->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stagingBuf, 1, &region);
                    });

                    // Read the result
                    void* mapped = VulkanAllocator::Map(stagingAlloc);
                    u32 entityIdx = *reinterpret_cast<u32*>(mapped);
                    VulkanAllocator::Unmap(stagingAlloc);
                    VulkanAllocator::FreeBuffer(stagingBuf, stagingAlloc);

                    if (entityIdx > 0 && entityIdx < (u32)m_EntityLookup.size())
                        m_PickedEntity = m_EntityLookup[entityIdx];
                    else
                        m_PickedEntity = entt::null;

                    m_PickResultReady = true;
                }
            }

            return;
        }
    }

    // =========================================================================
    // Frame Debugger — Snapshot
    // =========================================================================

    RG::RenderGraphSnapshot RenderingSystem::CaptureSnapshot(const RG::RenderGraph& rg)
    {
        RG::RenderGraphSnapshot snapshot;

        // Snapshot resources
        auto& resources = const_cast<RG::RenderGraph&>(rg).GetResources();
        snapshot.resources.reserve(resources.size());
        for (auto& res : resources)
        {
            RG::ResourceSnapshot rs;
            rs.name        = res.desc.name;
            rs.width       = res.desc.width;
            rs.height      = res.desc.height;
            rs.format      = res.desc.format;
            rs.isExternal  = res.external;
            rs.isTransient = res.isTransient;
            snapshot.resources.push_back(std::move(rs));
        }

        // Snapshot passes
        auto& passes = rg.GetPasses();
        snapshot.passes.reserve(passes.size());
        for (auto& pass : passes)
        {
            RG::PassSnapshot ps;
            ps.name                = pass.name;
            ps.culled              = pass.culled;
            ps.numColorAttachments = (u32)pass.colorAttachments.size();
            ps.hasDepth            = pass.hasDepth;

            for (auto& r : pass.reads)
            {
                RG::PassSnapshotResource sr;
                sr.index = r.index;
                sr.name  = (r.index > 0 && r.index <= resources.size()) ? resources[r.index - 1].desc.name : "?";
                ps.reads.push_back(std::move(sr));
            }

            for (auto& w : pass.writes)
            {
                RG::PassSnapshotResource sw;
                sw.index = w.index;
                sw.name  = (w.index > 0 && w.index <= resources.size()) ? resources[w.index - 1].desc.name : "?";
                ps.writes.push_back(std::move(sw));
            }

            // Compute primaryOutputIndex from first color write, or depth if depth-only pass
            if (!pass.colorAttachments.empty())
            {
                u32 idx = pass.colorAttachments[0].handle.index;
                if (idx > 0 && idx <= resources.size())
                    ps.primaryOutputIndex = (int)(idx - 1);
            }
            else if (pass.hasDepth)
            {
                u32 idx = pass.depthAttachment.handle.index;
                if (idx > 0 && idx <= resources.size())
                    ps.primaryOutputIndex = (int)(idx - 1);
            }

            snapshot.passes.push_back(std::move(ps));
        }

        // Compute geometry stats from draw command vectors (from previous frame's recording)
        u32 totalDraws = (u32)(m_OpaqueDraws.size() + m_CutoutDraws.size() + m_TransparentDraws.size());
        u32 totalIndices = 0;
        auto sumIndices = [&](const std::vector<DrawCommand>& draws) {
            for (auto& dc : draws)
            {
                if (!dc.model) continue;
                auto mesh = dc.model->GetMesh(dc.meshIndex);
                if (mesh && mesh->GetIndexBuffer())
                    totalIndices += mesh->GetIndexBuffer()->GetCount();
            }
        };
        sumIndices(m_OpaqueDraws);
        sumIndices(m_CutoutDraws);
        sumIndices(m_TransparentDraws);

        // Enrich per-pass pipeline state (known at RenderingSystem level, not RenderGraph)
        for (auto& ps : snapshot.passes)
        {
            if (ps.culled) continue;

            if (ps.name == "ShadowPass")
            {
                ps.depthTest = true; ps.depthWrite = true;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_FRONT_BIT;
                ps.shaderName = "shadowDepth";
                ps.drawCalls = totalDraws;
                ps.indices = totalIndices;
            }
            else if (ps.name == "GeometryPass")
            {
                ps.depthTest = true; ps.depthWrite = true;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_BACK_BIT;
                ps.shaderName = "pbr";
                ps.drawCalls = totalDraws;
                ps.indices = totalIndices;
            }
            else if (ps.name == "SkyboxPass")
            {
                ps.depthTest = true; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_BACK_BIT;
                ps.shaderName = "skybox";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "BloomExtract")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "bloomExtract";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "BloomBlurH" || ps.name == "BloomBlurV")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "bloomBlur";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "PostProcess")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = false;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "postprocess";
                ps.drawCalls = 1; ps.indices = 0;
            }
            else if (ps.name == "ImGuiPass")
            {
                ps.depthTest = false; ps.depthWrite = false;
                ps.blendEnabled = true;
                ps.cullMode = VK_CULL_MODE_NONE;
                ps.shaderName = "imgui";
                ps.drawCalls = 0; ps.indices = 0; // ImGui manages its own draws
            }
        }

        return snapshot;
    }

    void RenderingSystem::RegisterNamedTextures()
    {
        m_NamedTextures.clear();
        if (m_ShadowMap)      m_NamedTextures["ShadowMap"]      = m_ShadowMap;
        if (m_SceneColor)    m_NamedTextures["SceneColor"]    = m_SceneColor;
        if (m_SceneDepth)    m_NamedTextures["SceneDepth"]    = m_SceneDepth;
        if (m_LDROutput)     m_NamedTextures["LDROutput"]     = m_LDROutput;
        if (m_EntityIDBuffer)m_NamedTextures["EntityID"]     = m_EntityIDBuffer;
        if (m_BloomA)        m_NamedTextures["BloomA"]        = m_BloomA;
        if (m_BloomB)        m_NamedTextures["BloomB"]        = m_BloomB;
        if (m_IrradianceMap) m_NamedTextures["IrradianceMap"] = m_IrradianceMap;
        if (m_PrefilteredMap)m_NamedTextures["PrefilteredMap"]= m_PrefilteredMap;
        if (m_BRDFLut)       m_NamedTextures["BRDF_LUT"]     = m_BRDFLut;
    }

    std::shared_ptr<Texture> RenderingSystem::GetNamedTexture(const std::string& name) const
    {
        auto it = m_NamedTextures.find(name);
        return (it != m_NamedTextures.end()) ? it->second : nullptr;
    }

    // =========================================================================
    // Mouse Picking
    // =========================================================================

    void RenderingSystem::RequestPick(int x, int y)
    {
        m_PickCoord = { x, y };
        m_PickPending = true;
        m_PickResultReady = false;
    }

    entt::entity RenderingSystem::ConsumePickResult()
    {
        m_PickResultReady = false;
        return m_PickedEntity;
    }

    // =========================================================================
    // Resize
    // =========================================================================

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        // Guard against unsigned underflow from negative float→u32 casts at startup
        if (m_SceneColor && width > 0 && height > 0 && width <= 16384 && height <= 16384)
        {
            m_SceneColor    = Texture::Create(width, height, TextureFormat::RGBA16F);
            m_SceneDepth    = Texture::Create(width, height, TextureFormat::D32_Float);
            m_LDROutput     = Texture::Create(width, height, TextureFormat::RGBA8);
            m_EntityIDBuffer = Texture::Create(width, height, TextureFormat::R32_Uint);
            m_BloomA     = Texture::Create(std::max(width / 2, 1u), std::max(height / 2, 1u), TextureFormat::RGBA16F);
            m_BloomB     = Texture::Create(std::max(width / 2, 1u), std::max(height / 2, 1u), TextureFormat::RGBA16F);
            m_SelectionMask  = Texture::Create(width, height, TextureFormat::RGBA8);
            m_SelectionDepth = Texture::Create(width, height, TextureFormat::D32_Float);
            UpdatePostProcessDescriptors();

            // Update outline descriptors with new mask + depth buffers
            if (m_OutlineDescSet && m_OutlineSampler)
            {
                auto vkMask      = std::static_pointer_cast<VKTexture>(m_SelectionMask);
                auto vkSelDepth  = std::static_pointer_cast<VKTexture>(m_SelectionDepth);
                auto vkSceneDepth = std::static_pointer_cast<VKTexture>(m_SceneDepth);

                VkDescriptorImageInfo maskImgInfo{};
                maskImgInfo.sampler     = m_OutlineSampler;
                maskImgInfo.imageView   = vkMask->GetImageView();
                maskImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkDescriptorImageInfo selDepthImgInfo{};
                selDepthImgInfo.sampler     = m_OutlineSampler;
                selDepthImgInfo.imageView   = vkSelDepth->GetImageView();
                selDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkDescriptorImageInfo sceneDepthImgInfo{};
                sceneDepthImgInfo.sampler     = m_OutlineSampler;
                sceneDepthImgInfo.imageView   = vkSceneDepth->GetImageView();
                sceneDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet writes[3] = {};
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = m_OutlineDescSet;
                writes[0].dstBinding = 0;
                writes[0].descriptorCount = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[0].pImageInfo = &maskImgInfo;

                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = m_OutlineDescSet;
                writes[1].dstBinding = 1;
                writes[1].descriptorCount = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[1].pImageInfo = &selDepthImgInfo;

                writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[2].dstSet = m_OutlineDescSet;
                writes[2].dstBinding = 2;
                writes[2].descriptorCount = 1;
                writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[2].pImageInfo = &sceneDepthImgInfo;

                vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 3, writes, 0, nullptr);
            }

            // Update grid descriptor set: scene depth view changed on resize
            if (m_GridDescSet && m_GridDepthSampler)
            {
                auto vkSceneDepth = std::static_pointer_cast<VKTexture>(m_SceneDepth);

                VkDescriptorImageInfo gridDepthImgInfo{};
                gridDepthImgInfo.sampler     = m_GridDepthSampler;
                gridDepthImgInfo.imageView   = vkSceneDepth->GetImageView();
                gridDepthImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet gridWrite{};
                gridWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                gridWrite.dstSet = m_GridDescSet;
                gridWrite.dstBinding = 1;
                gridWrite.descriptorCount = 1;
                gridWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                gridWrite.pImageInfo = &gridDepthImgInfo;

                vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &gridWrite, 0, nullptr);
            }

            RegisterNamedTextures();
        }
    }

    // =========================================================================
    //  Frame Debugger: Re-Recording (Frozen State)
    // =========================================================================

    void RenderingSystem::InitDebugBlitResources()
    {
        if (m_FrameDebugger.blitPipeline) return; // Already initialized

        auto shadersPath = FileSystem::EngineAssetsPath("shaders");
        m_FrameDebugger.blitFragSpv  = ShaderCompiler::Compile(shadersPath / "debugBlit.frag");
        m_FrameDebugger.depthFragSpv = ShaderCompiler::Compile(shadersPath / "debugDepth.frag");

        if (m_FrameDebugger.blitFragSpv.empty() || m_FrameDebugger.depthFragSpv.empty() || m_FullscreenVertSpv.empty())
        {
            LH_CORE_ERROR("Failed to compile debug blit shaders");
            return;
        }

        auto device = VulkanContext::Get().GetDevice();

        // Create sampler
        VkSamplerCreateInfo samplerCI{};
        samplerCI.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerCI.magFilter = VK_FILTER_LINEAR;
        samplerCI.minFilter = VK_FILTER_LINEAR;
        samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(device, &samplerCI, nullptr, &m_FrameDebugger.sampler);

        // Descriptor set layout: binding 0 = combined image sampler
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCI{};
        layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutCI.bindingCount = 1;
        layoutCI.pBindings    = &binding;
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_FrameDebugger.descSetLayout);

        // Descriptor pool
        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo poolCI{};
        poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCI.maxSets       = 1;
        poolCI.poolSizeCount = 1;
        poolCI.pPoolSizes    = &poolSize;
        poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &poolCI, nullptr, &m_FrameDebugger.descPool);

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo allocCI{};
        allocCI.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocCI.descriptorPool     = m_FrameDebugger.descPool;
        allocCI.descriptorSetCount = 1;
        allocCI.pSetLayouts        = &m_FrameDebugger.descSetLayout;
        vkAllocateDescriptorSets(device, &allocCI, &m_FrameDebugger.descSet);

        // Create blit pipeline (color)
        std::vector<VkDescriptorSetLayout> layouts = { m_FrameDebugger.descSetLayout };
        PipelineConfig blitConfig;
        blitConfig.depthTest  = false;
        blitConfig.depthWrite = false;
        blitConfig.cullMode         = VK_CULL_MODE_NONE;
        blitConfig.colorFormats     = { VK_FORMAT_R8G8B8A8_UNORM };
        m_FrameDebugger.blitPipeline = std::make_unique<VKPipeline>(
            blitConfig, m_FullscreenVertSpv, m_FrameDebugger.blitFragSpv, layouts);

        // Create depth visualization pipeline
        PipelineConfig depthConfig;
        depthConfig.depthTest  = false;
        depthConfig.depthWrite = false;
        depthConfig.cullMode         = VK_CULL_MODE_NONE;
        depthConfig.colorFormats     = { VK_FORMAT_R8G8B8A8_UNORM };

        VkPushConstantRange depthPC{};
        depthPC.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        depthPC.offset     = 0;
        depthPC.size       = sizeof(float) * 2; // near, far
        depthConfig.pushConstantRanges = { depthPC };

        m_FrameDebugger.depthPipeline = std::make_unique<VKPipeline>(
            depthConfig, m_FullscreenVertSpv, m_FrameDebugger.depthFragSpv, layouts);
    }

    RG::ResourceHandle RenderingSystem::AddDebugBlitPass(RG::RenderGraph& rg, RG::ResourceHandle inputHandle, bool isDepth)
    {
        if (!m_FrameDebugger.blitPipeline || !m_LDROutput) return inputHandle;

        struct DebugBlitData {
            RG::ResourceHandle output;
            RG::ResourceHandle input;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<DebugBlitData>("DebugDisplayBlit",
            [&](DebugBlitData& data, RG::RenderPassBuilder& builder)
            {
                auto ldrVk = std::static_pointer_cast<VKTexture>(m_LDROutput);
                RG::TextureDesc desc;
                desc.name   = "LDROutput";
                desc.width  = m_LDROutput->GetWidth();
                desc.height = m_LDROutput->GetHeight();
                desc.format = RG::TextureFormat::RGBA8_Unorm;

                data.output = rg.ImportResource(desc,
                    (void*)ldrVk->GetImage(), (void*)ldrVk->GetImageView(),
                    RG::ResourceState::ShaderResource);
                data.output = builder.Write(data.output);

                data.input = builder.Read(inputHandle);
                outputHandle = data.output;
            },
            [this, isDepth](DebugBlitData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                u32 w = m_LDROutput->GetWidth();
                u32 h = m_LDROutput->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                if (isDepth && m_FrameDebugger.depthPipeline)
                {
                    m_FrameDebugger.depthPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_FrameDebugger.depthPipeline->GetLayout(), 0, 1, &m_FrameDebugger.descSet, 0, nullptr);

                    float pc[2] = { 0.1f, 200.0f }; // near/far for shadow maps
                    vkCmdPushConstants(cmd, m_FrameDebugger.depthPipeline->GetLayout(),
                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
                }
                else
                {
                    m_FrameDebugger.blitPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_FrameDebugger.blitPipeline->GetLayout(), 0, 1, &m_FrameDebugger.descSet, 0, nullptr);
                }

                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
        );

        return outputHandle;
    }

    void RenderingSystem::RenderCapturedFrame(u32 maxDrawCalls, Scene* scene)
    {
        if (!m_FrameDebugger.capturedFrame.valid) return;

        // Ensure debug blit resources are available
        InitDebugBlitResources();

        auto& registry = scene->Registry();
        m_FrameDebugger.replayDrawCounter = 0;

        RG::RenderGraph rg(*m_FrameAllocator);

        // Determine which pass the draw limit falls in, to know what render target is active
        std::string activeTarget = "SceneColor";
        bool isDepthTarget       = false;
        bool postProcessReached  = false;

        for (auto& cp : m_FrameDebugger.capturedFrame.passes)
        {
            u32 passEnd = cp.firstDrawIndex + cp.drawCallCount;
            if (cp.name == "PostProcess" && maxDrawCalls >= passEnd)
                postProcessReached = true;
            if (maxDrawCalls > cp.firstDrawIndex && maxDrawCalls <= passEnd)
            {
                activeTarget  = cp.activeRenderTarget;
                isDepthTarget = cp.isDepthTarget;
            }
            else if (maxDrawCalls > passEnd)
            {
                activeTarget  = cp.activeRenderTarget;
                isDepthTarget = cp.isDepthTarget;
            }
        }

        // --- Shadow Pass (replay) ---
        struct ReplayShadowData { RG::ResourceHandle shadowTex; };
        RG::ResourceHandle shadowHandle;

        rg.AddPass<ReplayShadowData>("ShadowPass",
            [&](ReplayShadowData& data, RG::RenderPassBuilder& builder)
            {
                auto vkShadowTex = std::static_pointer_cast<VKTexture>(m_ShadowMap);
                RG::TextureDesc desc;
                desc.name = "ShadowMap"; desc.width = 2048; desc.height = 2048;
                desc.format = RG::TextureFormat::D32_Float;

                data.shadowTex = rg.ImportResource(desc,
                    (void*)vkShadowTex->GetImage(), (void*)vkShadowTex->GetImageView(),
                    RG::ResourceState::Undefined);
                VkClearValue depthClear{}; depthClear.depthStencil = { 1.0f, 0 };
                data.shadowTex = builder.WriteDepth(data.shadowTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depthClear);
                shadowHandle = data.shadowTex;
            },
            [this, maxDrawCalls](ReplayShadowData& data, RG::RenderPassContext& ctx)
            {
                if (!m_ShadowPipeline) return;
                VkCommandBuffer cmd = ctx.commandBuffer;

                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = { m_GlobalDescriptorSet, bindlessSet, MaterialSystem::GetDescriptorSet(), m_LightDescSet, BoneMatrixBuffer::GetDescriptorSet() };
                m_ShadowPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_ShadowPipeline->GetLayout(), 0, 5, sets, 0, nullptr);

                VkViewport viewport{}; viewport.width = 2048.0f; viewport.height = 2048.0f; viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                VkRect2D scissor{}; scissor.extent = { 2048, 2048 };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                bool currentSkinned = false;

                // Re-draw using captured draw commands (all modes together for shadow)
                auto ReplayShadowDraws = [&](const std::vector<DrawCommand>& draws) {
                    for (const auto& dc : draws)
                    {
                        if (m_FrameDebugger.replayDrawCounter >= maxDrawCalls) return;

                        auto mesh = dc.model->GetMesh(dc.meshIndex);
                        if (!mesh) continue;
                        auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                        auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                        if (!vb || !ib) continue;

                        if (dc.isSkinned != currentSkinned)
                        {
                            currentSkinned = dc.isSkinned;
                            if (currentSkinned && m_ShadowSkinnedPipeline) {
                                m_ShadowSkinnedPipeline->Bind(cmd);
                                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_ShadowSkinnedPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                            } else {
                                m_ShadowPipeline->Bind(cmd);
                                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_ShadowPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                            }
                        }

                        VkPipelineLayout activeLayout = (currentSkinned && m_ShadowSkinnedPipeline)
                            ? m_ShadowSkinnedPipeline->GetLayout() : m_ShadowPipeline->GetLayout();

                        ObjectPushConstants pc{};
                        pc.modelMatrix   = dc.modelMatrix;
                        pc.materialIndex = 0;
                        pc.boneOffset    = dc.boneOffset;
                        vkCmdPushConstants(cmd, activeLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ObjectPushConstants), &pc);

                        VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                        VkDeviceSize offsets[] = { 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                        vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);
                        vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);
                        m_FrameDebugger.replayDrawCounter++;
                    }
                };

                ReplayShadowDraws(m_FrameDebugger.capturedOpaqueDraws);
                ReplayShadowDraws(m_FrameDebugger.capturedCutoutDraws);
                ReplayShadowDraws(m_FrameDebugger.capturedTransparentDraws);
            }
        );

        // --- Geometry Pass (replay) ---
        GeometryOutput geoOutput;
        {
            struct GeoReplayData {
                RG::ResourceHandle outputTex, entityIDTex, depthTex, shadowTex;
            };

            rg.AddPass<GeoReplayData>("GeometryPass",
                [&](GeoReplayData& data, RG::RenderPassBuilder& builder)
                {
                    auto vkTex = std::static_pointer_cast<VKTexture>(m_SceneColor);
                    RG::TextureDesc desc;
                    desc.name = "SceneColor"; desc.width = m_SceneColor->GetWidth(); desc.height = m_SceneColor->GetHeight();
                    desc.format = RG::TextureFormat::RGBA16_Float;
                    data.outputTex = rg.ImportResource(desc, (void*)vkTex->GetImage(), (void*)vkTex->GetImageView(), RG::ResourceState::ShaderResource);

                    auto vkID = std::static_pointer_cast<VKTexture>(m_EntityIDBuffer);
                    RG::TextureDesc idDesc;
                    idDesc.name = "EntityID"; idDesc.width = m_EntityIDBuffer->GetWidth(); idDesc.height = m_EntityIDBuffer->GetHeight();
                    idDesc.format = RG::TextureFormat::R32_Uint;
                    data.entityIDTex = rg.ImportResource(idDesc, (void*)vkID->GetImage(), (void*)vkID->GetImageView(), RG::ResourceState::Undefined);

                    auto vkDepth = std::static_pointer_cast<VKTexture>(m_SceneDepth);
                    RG::TextureDesc depthDesc;
                    depthDesc.name = "SceneDepth"; depthDesc.width = m_SceneDepth->GetWidth(); depthDesc.height = m_SceneDepth->GetHeight();
                    depthDesc.format = RG::TextureFormat::D32_Float;
                    data.depthTex = rg.ImportResource(depthDesc, (void*)vkDepth->GetImage(), (void*)vkDepth->GetImageView(), RG::ResourceState::Undefined);

                    VkClearValue depthClear{}; depthClear.depthStencil = { 1.0f, 0 };
                    data.depthTex    = builder.WriteDepth(data.depthTex, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depthClear);
                    data.outputTex   = builder.Write(data.outputTex);
                    VkClearValue idClear{}; idClear.color.uint32[0] = 0;
                    data.entityIDTex = builder.Write(data.entityIDTex, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, idClear);

                    if (shadowHandle.IsValid()) data.shadowTex = builder.Read(shadowHandle);

                    geoOutput.color = data.outputTex; geoOutput.depth = data.depthTex; geoOutput.entityID = data.entityIDTex;
                },
                [this, maxDrawCalls](GeoReplayData& data, RG::RenderPassContext& ctx)
                {
                    VkCommandBuffer cmd = ctx.commandBuffer;

                    UUID pbrUUID = ShaderLibrary::Get("pbr")->Handle;
                    VkPolygonMode polyMode = (m_ShadeMode == ShadeMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
                    auto* opaquePipeline = m_GeoPipelineManager.GetOrCreate(
                        pbrUUID, Material::RenderMode::Opaque, Material::CullMode::Back, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                    if (!opaquePipeline) return;
                    VkPipelineLayout pipelineLayout = opaquePipeline->GetLayout();

                    VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                    VkDescriptorSet sets[] = {
                        m_GlobalDescriptorSet, bindlessSet, MaterialSystem::GetDescriptorSet(),
                        m_LightDescSet, BoneMatrixBuffer::GetDescriptorSet(), m_ObjectSSBODescSet
                    };
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 6, sets, 0, nullptr);

                    RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.outputTex);
                    VkViewport viewport{}; viewport.width = (float)res->desc.width; viewport.height = (float)res->desc.height; viewport.maxDepth = 1.0f;
                    vkCmdSetViewport(cmd, 0, 1, &viewport);
                    VkRect2D scissor{}; scissor.extent = { res->desc.width, res->desc.height };
                    vkCmdSetScissor(cmd, 0, 1, &scissor);

                    // Replay bypasses GPU cull — reset instanceCount=1 for all captured objects
                    auto* indirectCmds = static_cast<VkDrawIndexedIndirectCommand*>(m_IndirectBufferMapped);

                    auto DrawBatchReplay = [&](const std::vector<DrawCommand>& draws, Material::RenderMode mode)
                    {
                        if (draws.empty() || m_FrameDebugger.replayDrawCounter >= maxDrawCalls) return;

                        Material::CullMode currentCull = Material::CullMode::Back;
                        bool currentSkinned = false;
                        auto* pipeline = m_GeoPipelineManager.GetOrCreate(pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                        if (!pipeline) return;
                        pipeline->Bind(cmd);

                        for (const auto& dc : draws)
                        {
                            if (m_FrameDebugger.replayDrawCounter >= maxDrawCalls) return;

                            if (dc.cullMode != currentCull || dc.isSkinned != currentSkinned)
                            {
                                currentCull = dc.cullMode; currentSkinned = dc.isSkinned;
                                VKPipeline* newPipeline = currentSkinned
                                    ? m_GeoSkinnedPipelineManager.GetOrCreate(pbrUUID, mode, currentCull, polyMode, m_PBRSkinnedVertSpv, m_PBRFragSpv)
                                    : m_GeoPipelineManager.GetOrCreate(pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                                if (!newPipeline) continue;
                                newPipeline->Bind(cmd);
                            }

                            auto mesh = dc.model->GetMesh(dc.meshIndex);
                            auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                            auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                            if (!vb || !ib) continue;

                            // Force visible for replay (GPU cull result may have zeroed instanceCount)
                            if (dc.gpuObjectIndex < m_GPUObjectCount)
                                indirectCmds[dc.gpuObjectIndex].instanceCount = 1;

                            VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                            VkDeviceSize offsets[] = { 0 };
                            vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                            vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);
                            VkDeviceSize indirectOffset = dc.gpuObjectIndex * sizeof(VkDrawIndexedIndirectCommand);
                            vkCmdDrawIndexedIndirect(cmd, m_IndirectBuffer, indirectOffset, 1, sizeof(VkDrawIndexedIndirectCommand));
                            m_FrameDebugger.replayDrawCounter++;
                        }
                    };

                    DrawBatchReplay(m_FrameDebugger.capturedOpaqueDraws,      Material::RenderMode::Opaque);
                    DrawBatchReplay(m_FrameDebugger.capturedCutoutDraws,      Material::RenderMode::Cutout);
                    DrawBatchReplay(m_FrameDebugger.capturedTransparentDraws, Material::RenderMode::Transparent);
                }
            );
        }

        // --- Skybox Pass (replay) ---
        RG::ResourceHandle skyboxColor = geoOutput.color;
        if (m_SkyboxPipeline && m_SkyboxVB && m_FrameDebugger.replayDrawCounter < maxDrawCalls)
        {
            struct SkyboxReplayData { RG::ResourceHandle colorTex, depthTex; };
            rg.AddPass<SkyboxReplayData>("SkyboxPass",
                [&](SkyboxReplayData& data, RG::RenderPassBuilder& builder) {
                    data.colorTex = builder.Write(geoOutput.color, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
                    data.depthTex = builder.WriteDepth(geoOutput.depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE);
                    skyboxColor = data.colorTex;
                },
                [this, maxDrawCalls](SkyboxReplayData& data, RG::RenderPassContext& ctx) {
                    if (m_FrameDebugger.replayDrawCounter >= maxDrawCalls) return;
                    VkCommandBuffer cmd = ctx.commandBuffer;
                    m_SkyboxPipeline->Bind(cmd);
                    VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                    VkDescriptorSet sets[] = { m_GlobalDescriptorSet, bindlessSet, MaterialSystem::GetDescriptorSet(), m_LightDescSet, BoneMatrixBuffer::GetDescriptorSet() };
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_SkyboxPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                    RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.colorTex);
                    VkViewport vp{}; vp.width = (float)res->desc.width; vp.height = (float)res->desc.height; vp.maxDepth = 1.0f;
                    vkCmdSetViewport(cmd, 0, 1, &vp);
                    VkRect2D sc{}; sc.extent = { res->desc.width, res->desc.height };
                    vkCmdSetScissor(cmd, 0, 1, &sc);
                    VkBuffer vb = m_SkyboxVB->GetVulkanBuffer(); VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
                    vkCmdDraw(cmd, 36, 1, 0, 0);
                    m_FrameDebugger.replayDrawCounter++;
                }
            );
        }

        // Determine final output handle for ImGui dependency
        RG::ResourceHandle finalOutput = skyboxColor;

        // --- Fullscreen passes (bloom, postprocess) - only if draw counter allows ---
        bool ppReached = false;
        if (m_FrameDebugger.replayDrawCounter < maxDrawCalls && m_BloomExtractPipeline && m_BloomBlurPipeline && m_BloomA && m_BloomB)
        {
            // Simplified: add bloom + postprocess as single draws each
            RG::ResourceHandle bloomResult = AddBloomPasses(rg, skyboxColor);
            if (m_FrameDebugger.replayDrawCounter + 3 < maxDrawCalls) // 3 bloom passes
            {
                m_FrameDebugger.replayDrawCounter += 3;
                if (m_PostProcessPipeline && m_LDROutput && m_FrameDebugger.replayDrawCounter < maxDrawCalls)
                {
                    finalOutput = AddPostProcessPass(rg, skyboxColor, bloomResult);
                    m_FrameDebugger.replayDrawCounter++;
                    ppReached = true;
                }
            }
        }

        // --- Rescue Blit: if we stopped before PostProcess ---
        if (!ppReached && m_FrameDebugger.blitPipeline)
        {
            // Update debug blit descriptor set to point to the active render target
            std::shared_ptr<Texture> activeTexture;
            bool activeIsDepth = isDepthTarget;

            if (activeTarget == "ShadowMap")       activeTexture = m_ShadowMap;
            else if (activeTarget == "SceneColor") activeTexture = m_SceneColor;
            else if (activeTarget == "BloomA" || activeTarget == "BloomAFinal")
                                                   activeTexture = m_BloomA;
            else if (activeTarget == "BloomB")     activeTexture = m_BloomB;
            else                                   activeTexture = m_SceneColor;

            if (activeTexture)
            {
                auto vkTex = std::static_pointer_cast<VKTexture>(activeTexture);
                VkDescriptorImageInfo imgInfo{};
                imgInfo.sampler     = m_FrameDebugger.sampler;
                imgInfo.imageView   = vkTex->GetImageView();
                imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet write{};
                write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet          = m_FrameDebugger.descSet;
                write.dstBinding      = 0;
                write.descriptorCount = 1;
                write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo      = &imgInfo;
                vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &write, 0, nullptr);

                // Need to get the active target into the render graph as a readable resource
                RG::TextureDesc inputDesc;
                inputDesc.name   = activeTarget;
                inputDesc.width  = activeTexture->GetWidth();
                inputDesc.height = activeTexture->GetHeight();
                inputDesc.format = activeIsDepth ? RG::TextureFormat::D32_Float : RG::TextureFormat::RGBA16_Float;

                RG::ResourceHandle inputHandle = rg.ImportResource(inputDesc,
                    (void*)vkTex->GetImage(), (void*)vkTex->GetImageView(),
                    RG::ResourceState::ShaderResource);

                finalOutput = AddDebugBlitPass(rg, inputHandle, activeIsDepth);
            }
        }

        // --- ImGui Pass (always runs) ---
        AddImGuiPass(rg, finalOutput);

        rg.Compile();

        // Update the graph snapshot for the panel display
        m_GraphSnapshot = CaptureSnapshot(rg);

        Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex(), nullptr);
    }
}
