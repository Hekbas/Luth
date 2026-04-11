#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/editor/Editor.h"
#include "luth/editor/EditorSelection.h"
#include "luth/editor/panels/ScenePanel.h"
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
#include "luth/renderer/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/VulkanShader.h"
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <stb/stb_image.h>
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
                for (const auto& [name, shader] : ShaderLibrary::GetAll()) {
                    if (shader->GetPath().stem().string() == stem) {
                        std::lock_guard lock(m_ReloadMutex);
                        m_PendingReloads.insert(name);
                        break;
                    }
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
        if (m_LightSetLayout)
            vkDestroyDescriptorSetLayout(device, m_LightSetLayout, nullptr);
        if (m_LightDescPool)
            vkDestroyDescriptorPool(device, m_LightDescPool, nullptr);
        if (m_GlobalSetLayout)
            vkDestroyDescriptorSetLayout(device, m_GlobalSetLayout, nullptr);
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

        // --- Shadow map (2048×2048, D32_Float, sampled) ---
        m_ShadowMap = Texture::Create(2048, 2048, TextureFormat::D32_Float);

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

    // Helper: run a one-shot compute dispatch with a single descriptor set
    struct ComputeDispatchInfo {
        VkDevice device;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<VkWriteDescriptorSet> writes;
        VkPushConstantRange pushConstantRange{};
        bool hasPushConstant = false;
    };

    static void RunComputeDispatch(
        const std::vector<u32>& spirv,
        VkDescriptorSetLayout descLayout,
        VkDescriptorSet descSet,
        u32 groupsX, u32 groupsY, u32 groupsZ,
        const void* pushData = nullptr, u32 pushSize = 0,
        VkPushConstantRange pushRange = {})
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // Create compute pipeline
        VkShaderModuleCreateInfo moduleInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        moduleInfo.codeSize = spirv.size() * sizeof(u32);
        moduleInfo.pCode = spirv.data();
        VkShaderModule shaderModule;
        vkCreateShaderModule(device, &moduleInfo, nullptr, &shaderModule);

        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descLayout;
        if (pushSize > 0) {
            layoutInfo.pushConstantRangeCount = 1;
            layoutInfo.pPushConstantRanges = &pushRange;
        }

        VkPipelineLayout pipelineLayout;
        vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout);

        VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shaderModule;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout;

        VkPipeline pipeline;
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

        VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descSet, 0, nullptr);
            if (pushData && pushSize > 0)
                vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize, pushData);
            vkCmdDispatch(cmd, groupsX, groupsY, groupsZ);
        });

        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyShaderModule(device, shaderModule, nullptr);
    }

    static void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
        VkAccessFlags srcAccess, VkAccessFlags dstAccess,
        VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
        u32 mipLevels, u32 layerCount, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
    {
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = aspect;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = mipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = layerCount;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void RenderingSystem::InitIBLResources(const fs::path& hdrPath)
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        auto shadersPath = FileSystem::EngineAssetsPath("shaders");

        // ---- 1. Load HDR environment map ----
        int hdrW, hdrH, hdrChannels;
        stbi_set_flip_vertically_on_load(1);
        float* hdrData = stbi_loadf(hdrPath.string().c_str(), &hdrW, &hdrH, &hdrChannels, 4);
        if (!hdrData) {
            LH_CORE_WARN("IBL: No HDR environment found at '{}'. IBL disabled.", hdrPath.string());
            // Create tiny fallback textures so descriptors are valid
            m_IrradianceMap  = std::make_shared<VKTexture>(1, 1, TextureFormat::RGBA16F, 6,
                VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, 1);
            m_PrefilteredMap = std::make_shared<VKTexture>(1, 1, TextureFormat::RGBA16F, 6,
                VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, 1);
            m_BRDFLut = std::make_shared<VKTexture>(1, 1, TextureFormat::RG16F, 1, 0, 1);
            goto write_descriptors;
        }
        LH_CORE_INFO("IBL: Loaded HDR environment {}x{} from '{}'", hdrW, hdrH, hdrPath.string());

        {
            // ---- 2. Upload HDR as 2D staging texture ----
            auto hdrStaging = std::make_shared<VKTexture>((u32)hdrW, (u32)hdrH, TextureFormat::RGBA32F,
                1, 0, 1, VkImageUsageFlags(0));
            // Upload pixel data
            {
                VkDeviceSize imageSize = (VkDeviceSize)hdrW * hdrH * 4 * sizeof(float);
                VkBuffer stagingBuffer;
                VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bufferInfo.size = imageSize;
                bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                VmaAllocation stagingAlloc = VulkanAllocator::AllocateBuffer(bufferInfo, VMA_MEMORY_USAGE_CPU_ONLY, stagingBuffer);
                void* mapped = VulkanAllocator::Map(stagingAlloc);
                memcpy(mapped, hdrData, (size_t)imageSize);
                VulkanAllocator::Unmap(stagingAlloc);

                VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                    TransitionImage(cmd, hdrStaging->GetImage(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, 1);

                    VkBufferImageCopy region{};
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.imageSubresource.layerCount = 1;
                    region.imageExtent = { (u32)hdrW, (u32)hdrH, 1 };
                    vkCmdCopyBufferToImage(cmd, stagingBuffer, hdrStaging->GetImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                    TransitionImage(cmd, hdrStaging->GetImage(),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, 1);
                });
                VulkanAllocator::FreeBuffer(stagingBuffer, stagingAlloc);
            }
            stbi_image_free(hdrData);

            // ---- 3. Create environment cubemap (1024x1024) ----
            const u32 envSize = 1024;
            const u32 envMips = static_cast<u32>(std::floor(std::log2(envSize))) + 1;
            auto envCubemap = std::make_shared<VKTexture>(envSize, envSize, TextureFormat::RGBA16F, 6,
                VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, envMips, VK_IMAGE_USAGE_STORAGE_BIT);

            // ---- 4. Equirect → Cubemap conversion ----
            {
                auto spv = ShaderCompiler::Compile(shadersPath / "equirect_to_cubemap.comp");

                // Descriptor set: binding 0 = sampler2D (HDR), binding 1 = image2DArray (cubemap)
                VkDescriptorSetLayoutBinding layoutBindings[2] = {};
                layoutBindings[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
                layoutBindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

                VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                layoutCI.bindingCount = 2;
                layoutCI.pBindings = layoutBindings;
                VkDescriptorSetLayout descLayout;
                vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &descLayout);

                VkDescriptorSet descSet;
                VulkanContext::Get().GetDescriptorAllocator().Allocate(descLayout, descSet);

                // Create sampler for HDR input
                VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                sampCI.magFilter = VK_FILTER_LINEAR;
                sampCI.minFilter = VK_FILTER_LINEAR;
                sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                VkSampler hdrSampler;
                vkCreateSampler(device, &sampCI, nullptr, &hdrSampler);

                // Transition cubemap to GENERAL for compute writes
                VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                    TransitionImage(cmd, envCubemap->GetImage(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        0, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        envMips, 6);
                });

                // Write mip 0 view for storage image (forStorage=true → 2D_ARRAY for compute)
                VkImageView envMip0View = envCubemap->CreateMipView(0, true);

                VkDescriptorImageInfo hdrInfo{};
                hdrInfo.sampler = hdrSampler;
                hdrInfo.imageView = hdrStaging->GetImageView();
                hdrInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkDescriptorImageInfo cubemapInfo{};
                cubemapInfo.imageView = envMip0View;
                cubemapInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                VkWriteDescriptorSet writes[2] = {};
                writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[0].dstSet = descSet;
                writes[0].dstBinding = 0;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[0].descriptorCount = 1;
                writes[0].pImageInfo = &hdrInfo;

                writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[1].dstSet = descSet;
                writes[1].dstBinding = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[1].descriptorCount = 1;
                writes[1].pImageInfo = &cubemapInfo;

                vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

                RunComputeDispatch(spv, descLayout, descSet,
                    (envSize + 15) / 16, (envSize + 15) / 16, 6);

                // Transition cubemap to TRANSFER_DST for mipmap generation
                VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                    TransitionImage(cmd, envCubemap->GetImage(),
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        envMips, 6);
                });

                // Generate mipmaps for environment cubemap
                VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                    i32 mipW = envSize, mipH = envSize;
                    for (u32 i = 1; i < envMips; i++) {
                        // Transition mip i-1 to TRANSFER_SRC
                        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        barrier.image = envCubemap->GetImage();
                        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 6 };
                        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &barrier);

                        i32 nextW = mipW > 1 ? mipW / 2 : 1;
                        i32 nextH = mipH > 1 ? mipH / 2 : 1;

                        VkImageBlit blit{};
                        blit.srcOffsets[0] = { 0, 0, 0 };
                        blit.srcOffsets[1] = { mipW, mipH, 1 };
                        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 6 };
                        blit.dstOffsets[0] = { 0, 0, 0 };
                        blit.dstOffsets[1] = { nextW, nextH, 1 };
                        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 6 };
                        vkCmdBlitImage(cmd, envCubemap->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            envCubemap->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1, &blit, VK_FILTER_LINEAR);

                        // Transition mip i-1 to SHADER_READ_ONLY
                        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &barrier);

                        mipW = nextW;
                        mipH = nextH;
                    }
                    // Last mip: DST → SHADER_READ_ONLY
                    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    barrier.image = envCubemap->GetImage();
                    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, envMips - 1, 1, 0, 6 };
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
                });

                vkDestroyImageView(device, envMip0View, nullptr);
                vkDestroySampler(device, hdrSampler, nullptr);
                vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
            }

            // ---- 5. Irradiance convolution (32x32 cubemap) ----
            {
                const u32 irrSize = 32;
                m_IrradianceMap = std::make_shared<VKTexture>(irrSize, irrSize, TextureFormat::RGBA16F, 6,
                    VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, 1, VK_IMAGE_USAGE_STORAGE_BIT);

                auto spv = ShaderCompiler::Compile(shadersPath / "irradiance_convolve.comp");

                VkDescriptorSetLayoutBinding layoutBindings[2] = {};
                layoutBindings[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
                layoutBindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

                VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                layoutCI.bindingCount = 2;
                layoutCI.pBindings = layoutBindings;
                VkDescriptorSetLayout descLayout;
                vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &descLayout);

                VkDescriptorSet descSet;
                VulkanContext::Get().GetDescriptorAllocator().Allocate(descLayout, descSet);

                // Create sampler for env cubemap input
                VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                sampCI.magFilter = VK_FILTER_LINEAR;
                sampCI.minFilter = VK_FILTER_LINEAR;
                sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                sampCI.maxLod = (float)envMips;
                VkSampler envSampler;
                vkCreateSampler(device, &sampCI, nullptr, &envSampler);

                // Transition irradiance to GENERAL
                VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                    TransitionImage(cmd, std::static_pointer_cast<VKTexture>(m_IrradianceMap)->GetImage(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        0, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, 6);
                });

                VkDescriptorImageInfo envInfo{};
                envInfo.sampler = envSampler;
                envInfo.imageView = envCubemap->GetImageView();
                envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                // Create 2D_ARRAY view for compute storage (not CUBE)
                auto vkIrr = std::static_pointer_cast<VKTexture>(m_IrradianceMap);
                VkImageView irrStorageView = vkIrr->CreateMipView(0, true);

                VkDescriptorImageInfo irrInfo{};
                irrInfo.imageView = irrStorageView;
                irrInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                VkWriteDescriptorSet writes[2] = {};
                writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[0].dstSet = descSet;
                writes[0].dstBinding = 0;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[0].descriptorCount = 1;
                writes[0].pImageInfo = &envInfo;
                writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                writes[1].dstSet = descSet;
                writes[1].dstBinding = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[1].descriptorCount = 1;
                writes[1].pImageInfo = &irrInfo;
                vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

                RunComputeDispatch(spv, descLayout, descSet,
                    (irrSize + 7) / 8, (irrSize + 7) / 8, 6);

                vkDestroyImageView(device, irrStorageView, nullptr);

                // Transition irradiance to SHADER_READ_ONLY
                VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                    TransitionImage(cmd, std::static_pointer_cast<VKTexture>(m_IrradianceMap)->GetImage(),
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1, 6);
                });

                vkDestroySampler(device, envSampler, nullptr);
                vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
            }

            // ---- 6. Pre-filtered environment map (128x128, 5 mip levels) ----
            {
                const u32 pfSize = 128;
                const u32 pfMips = 5;
                m_PrefilteredMap = std::make_shared<VKTexture>(pfSize, pfSize, TextureFormat::RGBA16F, 6,
                    VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, pfMips, VK_IMAGE_USAGE_STORAGE_BIT);

                auto spv = ShaderCompiler::Compile(shadersPath / "prefilter_env.comp");

                VkDescriptorSetLayoutBinding layoutBindings[2] = {};
                layoutBindings[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
                layoutBindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

                VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                layoutCI.bindingCount = 2;
                layoutCI.pBindings = layoutBindings;
                VkDescriptorSetLayout descLayout;
                vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &descLayout);

                VkPushConstantRange pcRange{};
                pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                pcRange.offset = 0;
                pcRange.size = sizeof(float);

                // Create sampler for env cubemap input
                VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                sampCI.magFilter = VK_FILTER_LINEAR;
                sampCI.minFilter = VK_FILTER_LINEAR;
                sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                sampCI.maxLod = (float)envMips;
                VkSampler envSampler;
                vkCreateSampler(device, &sampCI, nullptr, &envSampler);

                auto vkPf = std::static_pointer_cast<VKTexture>(m_PrefilteredMap);

                // Transition all mips to GENERAL
                VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                    TransitionImage(cmd, vkPf->GetImage(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        0, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        pfMips, 6);
                });

                // Dispatch once per mip level
                for (u32 mip = 0; mip < pfMips; mip++)
                {
                    u32 mipSize = pfSize >> mip;
                    float roughness = (float)mip / (float)(pfMips - 1);

                    VkImageView mipView = vkPf->CreateMipView(mip, true);

                    VkDescriptorSet descSet;
                    VulkanContext::Get().GetDescriptorAllocator().Allocate(descLayout, descSet);

                    VkDescriptorImageInfo envInfo{};
                    envInfo.sampler = envSampler;
                    envInfo.imageView = envCubemap->GetImageView();
                    envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                    VkDescriptorImageInfo pfInfo{};
                    pfInfo.imageView = mipView;
                    pfInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                    VkWriteDescriptorSet writes[2] = {};
                    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[0].dstSet = descSet;
                    writes[0].dstBinding = 0;
                    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[0].descriptorCount = 1;
                    writes[0].pImageInfo = &envInfo;
                    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[1].dstSet = descSet;
                    writes[1].dstBinding = 1;
                    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    writes[1].descriptorCount = 1;
                    writes[1].pImageInfo = &pfInfo;
                    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

                    RunComputeDispatch(spv, descLayout, descSet,
                        (mipSize + 15) / 16, (mipSize + 15) / 16, 6,
                        &roughness, sizeof(float), pcRange);

                    vkDestroyImageView(device, mipView, nullptr);
                }

                // Transition prefiltered to SHADER_READ_ONLY
                VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                    TransitionImage(cmd, vkPf->GetImage(),
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        pfMips, 6);
                });

                vkDestroySampler(device, envSampler, nullptr);
                vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
            }

            // ---- 7. BRDF LUT (512x512, RG16F) ----
            {
                const u32 lutSize = 512;
                m_BRDFLut = std::make_shared<VKTexture>(lutSize, lutSize, TextureFormat::RG16F, 1, 0, 1,
                    VK_IMAGE_USAGE_STORAGE_BIT);

                auto spv = ShaderCompiler::Compile(shadersPath / "brdf_lut.comp");

                VkDescriptorSetLayoutBinding layoutBinding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

                VkDescriptorSetLayoutCreateInfo layoutCI{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                layoutCI.bindingCount = 1;
                layoutCI.pBindings = &layoutBinding;
                VkDescriptorSetLayout descLayout;
                vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &descLayout);

                VkDescriptorSet descSet;
                VulkanContext::Get().GetDescriptorAllocator().Allocate(descLayout, descSet);

                auto vkLut = std::static_pointer_cast<VKTexture>(m_BRDFLut);

                // Transition to GENERAL
                VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                    TransitionImage(cmd, vkLut->GetImage(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        0, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, 1);
                });

                VkDescriptorImageInfo lutInfo{};
                lutInfo.imageView = vkLut->GetImageView();
                lutInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                write.dstSet = descSet;
                write.dstBinding = 0;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write.descriptorCount = 1;
                write.pImageInfo = &lutInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

                RunComputeDispatch(spv, descLayout, descSet,
                    (lutSize + 15) / 16, (lutSize + 15) / 16, 1);

                // Transition to SHADER_READ_ONLY
                VulkanContext::Get().ImmediateSubmit([&](VkCommandBuffer cmd) {
                    TransitionImage(cmd, vkLut->GetImage(),
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 1, 1);
                });

                vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
            }

            // envCubemap goes out of scope here — temp resource freed
        }

    write_descriptors:
        // ---- 8. IBL sampler ----
        {
            VkSamplerCreateInfo sampCI{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sampCI.magFilter = VK_FILTER_LINEAR;
            sampCI.minFilter = VK_FILTER_LINEAR;
            sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sampCI.maxLod = 4.0f;
            vkCreateSampler(device, &sampCI, nullptr, &m_IBLSampler);
        }

        // ---- 9. Write IBL descriptors to Set 0 (bindings 1-3) ----
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

        // ---- 10. Skybox cube mesh + shader compilation ----
        {
            // Unit cube (36 vertices, position only)
            float cubeVertices[] = {
                // +X
                 1, -1, -1,   1, -1,  1,   1,  1,  1,   1,  1,  1,   1,  1, -1,   1, -1, -1,
                // -X
                -1, -1,  1,  -1, -1, -1,  -1,  1, -1,  -1,  1, -1,  -1,  1,  1,  -1, -1,  1,
                // +Y
                -1,  1, -1,   1,  1, -1,   1,  1,  1,   1,  1,  1,  -1,  1,  1,  -1,  1, -1,
                // -Y
                -1, -1,  1,   1, -1,  1,   1, -1, -1,   1, -1, -1,  -1, -1, -1,  -1, -1,  1,
                // +Z
                -1, -1,  1,  -1,  1,  1,   1,  1,  1,   1,  1,  1,   1, -1,  1,  -1, -1,  1,
                // -Z
                 1, -1, -1,   1,  1, -1,  -1,  1, -1,  -1,  1, -1,  -1, -1, -1,   1, -1, -1,
            };
            m_SkyboxVB = std::make_shared<VKVertexBuffer>(cubeVertices, sizeof(cubeVertices));

            // Compile skybox shaders
            m_SkyboxVertSpv = ShaderCompiler::Compile(shadersPath / "skybox.vert");
            m_SkyboxFragSpv = ShaderCompiler::Compile(shadersPath / "skybox.frag");
            if (m_SkyboxVertSpv.empty() || m_SkyboxFragSpv.empty())
                LH_CORE_ERROR("Failed to compile skybox shaders!");
        }

        LH_CORE_INFO("IBL: Precomputation complete (irradiance 32x32, prefiltered 128x128, BRDF LUT 512x512)");
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

    void RenderingSystem::CreatePipelines()
    {
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

        // 5-set layout shared by geometry/shadow/skybox pipelines
        std::vector<VkDescriptorSetLayout> layouts = {
            m_GlobalSetLayout,                                    // Set 0
            VulkanContext::Get().GetBindlessSet().GetLayout(),   // Set 1
            MaterialSystem::GetDescriptorSetLayout(),            // Set 2
            m_LightSetLayout,                                    // Set 3
            BoneMatrixBuffer::GetDescriptorSetLayout()           // Set 4
        };

        // ---- PBR geometry pipeline manager (lazy creation keyed by {shaderUUID, renderMode}) ----
        m_GeoPipelineManager.Init(layouts,
            [bindingDescs, attribDescs, pushConstantRange](Material::RenderMode mode, Material::CullMode cullMode, VkPolygonMode polygonMode) -> PipelineConfig
            {
                PipelineConfig config;
                config.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_UINT };
                config.depthFormat = VK_FORMAT_D32_SFLOAT;
                config.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                config.bindingDescriptions = bindingDescs;
                config.attributeDescriptions = attribDescs;
                config.pushConstantRanges = { pushConstantRange };
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

        PipelineConfig shadowConfig;
        shadowConfig.colorFormats = {};  // depth-only
        shadowConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
        shadowConfig.depthTest = true; shadowConfig.depthWrite = true;
        shadowConfig.blendEnabled = false;
        shadowConfig.cullMode = VK_CULL_MODE_FRONT_BIT; // front-face culling reduces shadow acne
        shadowConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        shadowConfig.bindingDescriptions = shadowBindingDescs;
        shadowConfig.attributeDescriptions = shadowAttribDescs;
        shadowConfig.pushConstantRanges = { pushConstantRange };

        m_ShadowPipeline = std::make_unique<VKPipeline>(shadowConfig, m_ShadowVertSpv, m_ShadowFragSpv, layouts);

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

        m_GeoSkinnedPipelineManager.Init(layouts,
            [skinnedBindingDescs, skinnedAttribDescs, pushConstantRange](Material::RenderMode mode, Material::CullMode cullMode, VkPolygonMode polygonMode) -> PipelineConfig
            {
                PipelineConfig config;
                config.colorFormats = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_UINT };
                config.depthFormat = VK_FORMAT_D32_SFLOAT;
                config.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                config.bindingDescriptions = skinnedBindingDescs;
                config.attributeDescriptions = skinnedAttribDescs;
                config.pushConstantRanges = { pushConstantRange };
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
            shadowSkinnedConfig.pushConstantRanges = { pushConstantRange };

            m_ShadowSkinnedPipeline = std::make_unique<VKPipeline>(
                shadowSkinnedConfig, m_ShadowSkinnedVertSpv, m_ShadowFragSpv, layouts);
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

    void RenderingSystem::UpdateLightUniforms(Scene* scene)
    {
        auto& registry = scene->Registry();
        LightUniforms lights{};

        // Directional light — use first entity found
        bool foundDir = false;
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
                m_CachedShadowBias   = dl.ShadowBias;
                m_CachedShadowOrtho  = dl.ShadowOrthoSize;
                m_CachedShadowDist   = dl.ShadowDistance;
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

        // Compute light-space matrix (orthographic from directional light)
        float orthoSize = m_CachedShadowOrtho;
        float shadowDist = m_CachedShadowDist;
        auto scenePanel = Editor::GetPanel<ScenePanel>();
        glm::vec3 camPos = scenePanel ? scenePanel->GetEditorCamera().GetPosition() : glm::vec3(0.0f);

        glm::vec3 lightDir = lights.dirLight.direction;
        glm::vec3 lightPos = camPos - lightDir * shadowDist;
        glm::vec3 up = (glm::abs(glm::dot(lightDir, glm::vec3(0, 1, 0))) > 0.99f)
                       ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);

        glm::mat4 lightView = glm::lookAt(lightPos, lightPos + lightDir, up);
        glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, -shadowDist, shadowDist * 2.0f);
        lightProj[1][1] *= -1.0f; // Vulkan Y-flip

        m_CachedLightSpaceMatrix = lightProj * lightView;
    }

    void RenderingSystem::UpdateGlobalUniforms()
    {
        auto scenePanel = Editor::GetPanel<ScenePanel>();
        if (!scenePanel) return;

        EditorCamera& camera = scenePanel->GetEditorCamera();

        GlobalUniforms ubo{};
        ubo.view = camera.GetViewMatrix();
        ubo.projection = camera.GetProjectionMatrix();
        ubo.projection[1][1] *= -1.0f;  // Vulkan Y-flip (shader only, not ImGuizmo)
        ubo.viewProjection = ubo.projection * ubo.view;
        ubo.cameraPos = camera.GetPosition();
        ubo.time = Time::GetTime();
        ubo.lightSpaceMatrix = m_CachedLightSpaceMatrix;
        ubo.shadowBias = m_CachedCastShadows ? m_CachedShadowBias : -1.0f; // negative = shadows disabled
        ubo.iblIntensity    = Editor::GetSettings().iblIntensity;
        ubo.skyboxIntensity = Editor::GetSettings().skyboxIntensity;

        m_GlobalUniformBuffer->SetData(&ubo, sizeof(GlobalUniforms));
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
        }

        m_FrameAllocator->Reset();

        // --- Frame Debugger: Frozen state → re-render captured frame ---
        if (m_DebuggerState == DebuggerState::Frozen)
        {
            if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan)
            {
                UpdateGlobalUniforms(); // camera may have moved
                RenderCapturedFrame(m_DebuggerDrawLimit, scene);
            }
            return;
        }

        // --- Frame Debugger: Prepare for capture ---
        if (m_DebuggerState == DebuggerState::CaptureRequested)
            m_CapturedFrame.Clear();

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

            RG::RenderGraph rg(*m_FrameAllocator);

            RG::ResourceHandle shadowMap   = AddShadowPass(rg, registry);
            auto geoOutput                 = AddGeometryPass(rg, registry, shadowMap);
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
            if (m_DebuggerState == DebuggerState::CaptureRequested)
            {
                // Copy draw command vectors for re-recording
                m_CapturedOpaqueDraws      = m_OpaqueDraws;
                m_CapturedCutoutDraws      = m_CutoutDraws;
                m_CapturedTransparentDraws = m_TransparentDraws;

                // Copy resource and timing info from the graph snapshot
                m_CapturedFrame.resources      = m_GraphSnapshot.resources;
                m_CapturedFrame.totalGpuTimeMs = m_GraphSnapshot.totalGpuTimeMs;

                // Copy per-pass GPU times into captured passes
                {
                    u32 capturedIdx = 0;
                    for (auto& ps : m_GraphSnapshot.passes)
                    {
                        if (ps.culled) continue;
                        if (capturedIdx < m_CapturedFrame.passes.size())
                            m_CapturedFrame.passes[capturedIdx].gpuTimeMs = ps.gpuTimeMs;
                        capturedIdx++;
                    }
                }

                m_CapturedFrame.valid = true;
                m_DebuggerState       = DebuggerState::Frozen;
                m_DebuggerDrawLimit   = (u32)m_CapturedFrame.drawCalls.size();
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
    // Render Graph Passes
    // =========================================================================

    RG::ResourceHandle RenderingSystem::AddShadowPass(RG::RenderGraph& rg, entt::registry& registry)
    {
        struct ShadowPassData {
            RG::ResourceHandle shadowTex;
        };

        RG::ResourceHandle shadowHandle;

        rg.AddPass<ShadowPassData>("ShadowPass",
            [&](ShadowPassData& data, RG::RenderPassBuilder& builder)
            {
                auto vkShadowTex = std::static_pointer_cast<VKTexture>(m_ShadowMap);

                RG::TextureDesc desc;
                desc.name   = "ShadowMap";
                desc.width  = 2048;
                desc.height = 2048;
                desc.format = RG::TextureFormat::D32_Float;

                data.shadowTex = rg.ImportResource(desc,
                    (void*)vkShadowTex->GetImage(),
                    (void*)vkShadowTex->GetImageView(),
                    RG::ResourceState::Undefined);

                VkClearValue depthClear{};
                depthClear.depthStencil = { 1.0f, 0 };
                // STORE so the depth values are kept for the geometry pass to read
                data.shadowTex = builder.WriteDepth(data.shadowTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depthClear);

                shadowHandle = data.shadowTex;
            },
            [this, &registry](ShadowPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                BeginCapturePass("ShadowPass", "ShadowMap", true,
                    { "shadowDepth", 0, VK_CULL_MODE_FRONT_BIT, VK_POLYGON_MODE_FILL, false, true, true, false });

                if (!m_ShadowPipeline) { LH_CORE_ERROR("Shadow pipeline is null!"); EndCapturePass(); return; }

                // Bind all 5 descriptor sets
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_GlobalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet,
                    BoneMatrixBuffer::GetDescriptorSet()
                };

                // Start with static pipeline bound
                m_ShadowPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_ShadowPipeline->GetLayout(), 0, 5, sets, 0, nullptr);

                // Shadow map viewport
                VkViewport viewport{};
                viewport.width    = 2048.0f;
                viewport.height   = 2048.0f;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { 2048, 2048 };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                bool currentSkinned = false;

                auto view = registry.view<WorldTransform, MeshRenderer>();
                for (auto [entity, worldTransform, meshRenderer] : view.each())
                {
                    auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID);
                    if (!model) continue;
                    auto mesh = model->GetMesh(meshRenderer.MeshIndex);
                    if (!mesh) continue;

                    auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                    auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                    if (!vb || !ib) continue;

                    // Check per-mesh skinning
                    bool isSkinned = false;
                    u32 boneOffset = 0;
                    if (meshRenderer.MeshIndex < model->GetMeshesData().size())
                        isSkinned = model->GetMeshesData()[meshRenderer.MeshIndex].IsSkinned;

                    if (isSkinned)
                    {
                        // Find Animation on this entity or parent
                        entt::entity animEntity = entt::null;
                        if (registry.any_of<Component::Animation>(entity))
                            animEntity = entity;
                        else if (registry.any_of<Component::Parent>(entity)) {
                            auto parentEnt = (entt::entity)registry.get<Component::Parent>(entity).m_Parent;
                            if (registry.valid(parentEnt) && registry.any_of<Component::Animation>(parentEnt))
                                animEntity = parentEnt;
                        }
                        if (animEntity != entt::null) {
                            auto& anim = registry.get<Component::Animation>(animEntity);
                            if (anim.BufferAllocated)
                                boneOffset = anim.BoneBufferOffset;
                        }
                    }

                    // Switch pipeline if skinned state changed
                    if (isSkinned != currentSkinned)
                    {
                        currentSkinned = isSkinned;
                        if (isSkinned && m_ShadowSkinnedPipeline)
                        {
                            m_ShadowSkinnedPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_ShadowSkinnedPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                        }
                        else
                        {
                            m_ShadowPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_ShadowPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                        }
                    }

                    VkPipelineLayout activeLayout = (currentSkinned && m_ShadowSkinnedPipeline)
                        ? m_ShadowSkinnedPipeline->GetLayout()
                        : m_ShadowPipeline->GetLayout();

                    ObjectPushConstants pc{};
                    pc.modelMatrix   = worldTransform.Matrix;
                    pc.materialIndex = 0;
                    pc.boneOffset    = boneOffset;

                    vkCmdPushConstants(cmd, activeLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(ObjectPushConstants), &pc);

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);

                    // Capture draw call for frame debugger
                    CaptureDrawCall("ShadowPass",
                        model->GetName() + "[" + std::to_string(meshRenderer.MeshIndex) + "]",
                        registry.any_of<Component::Tag>(entity) ? registry.get<Component::Tag>(entity).m_Tag : "Entity",
                        0, ib->GetCount(), pc,
                        { "shadowDepth", 0, static_cast<u32>(VK_CULL_MODE_FRONT_BIT),
                          VK_POLYGON_MODE_FILL, isSkinned, true, true, false });
                }

                EndCapturePass();
            }
        );

        return shadowHandle;
    }

    GeometryOutput RenderingSystem::AddGeometryPass(
        RG::RenderGraph& rg, entt::registry& registry, RG::ResourceHandle shadowMapHandle)
    {
        struct GeometryPassData {
            RG::ResourceHandle outputTex;
            RG::ResourceHandle entityIDTex;
            RG::ResourceHandle depthTex;
            RG::ResourceHandle shadowTex;
        };

        GeometryOutput output;

        rg.AddPass<GeometryPassData>("GeometryPass",
            [&](GeometryPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "SceneColor";
                desc.width  = m_SceneColor->GetWidth();
                desc.height = m_SceneColor->GetHeight();
                desc.format = RG::TextureFormat::RGBA16_Float;

                auto vkTex = std::static_pointer_cast<VKTexture>(m_SceneColor);
                data.outputTex = rg.ImportResource(desc,
                    (void*)vkTex->GetImage(),
                    (void*)vkTex->GetImageView(),
                    RG::ResourceState::ShaderResource);

                // Entity ID buffer (R32_UINT)
                RG::TextureDesc idDesc;
                idDesc.name   = "EntityID";
                idDesc.width  = m_EntityIDBuffer->GetWidth();
                idDesc.height = m_EntityIDBuffer->GetHeight();
                idDesc.format = RG::TextureFormat::R32_Uint;

                auto vkID = std::static_pointer_cast<VKTexture>(m_EntityIDBuffer);
                data.entityIDTex = rg.ImportResource(idDesc,
                    (void*)vkID->GetImage(),
                    (void*)vkID->GetImageView(),
                    RG::ResourceState::Undefined);

                RG::TextureDesc depthDesc;
                depthDesc.name   = "SceneDepth";
                depthDesc.width  = m_SceneDepth->GetWidth();
                depthDesc.height = m_SceneDepth->GetHeight();
                depthDesc.format = RG::TextureFormat::D32_Float;

                auto vkDepth = std::static_pointer_cast<VKTexture>(m_SceneDepth);
                data.depthTex = rg.ImportResource(depthDesc,
                    (void*)vkDepth->GetImage(),
                    (void*)vkDepth->GetImageView(),
                    RG::ResourceState::Undefined);

                VkClearValue depthClear{};
                depthClear.depthStencil = { 1.0f, 0 };
                data.depthTex  = builder.WriteDepth(data.depthTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depthClear);
                data.outputTex = builder.Write(data.outputTex);

                VkClearValue idClear{};
                idClear.color.uint32[0] = 0;
                data.entityIDTex = builder.Write(data.entityIDTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, idClear);

                // Declare dependency on the shadow map (triggers depth→shader_read barrier)
                if (shadowMapHandle.IsValid())
                    data.shadowTex = builder.Read(shadowMapHandle);

                output.color    = data.outputTex;
                output.depth    = data.depthTex;
                output.entityID = data.entityIDTex;
            },
            [this, &registry](GeometryPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                VkPolygonMode polyMode = (m_ShadeMode == ShadeMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
                BeginCapturePass("GeometryPass", "SceneColor", false,
                    { "pbr", 0, VK_CULL_MODE_BACK_BIT, polyMode, false, true, true, false });

                UUID pbrUUID = ShaderLibrary::Get("pbr")->Handle;
                auto* opaquePipeline = m_GeoPipelineManager.GetOrCreate(
                    pbrUUID, Material::RenderMode::Opaque, Material::CullMode::Back, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                if (!opaquePipeline) { EndCapturePass(); return; }
                VkPipelineLayout pipelineLayout = opaquePipeline->GetLayout();

                // Bind all 5 descriptor sets
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_GlobalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet,
                    BoneMatrixBuffer::GetDescriptorSet()
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout, 0, 5, sets, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.outputTex);

                VkViewport viewport{};
                viewport.width    = (float)res->desc.width;
                viewport.height   = (float)res->desc.height;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                // Collect draw commands per render mode (reuse member vectors to avoid per-frame heap alloc)
                m_OpaqueDraws.clear();
                m_CutoutDraws.clear();
                m_TransparentDraws.clear();
                m_VisibleTriCount = 0;

                // Build entity lookup table (index 0 = null sentinel)
                m_EntityLookup.clear();
                m_EntityLookup.push_back(entt::null);

                auto view = registry.view<WorldTransform, MeshRenderer>();
                for (auto [entity, worldTransform, meshRenderer] : view.each())
                {
                    auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID);
                    if (!model) continue;
                    auto mesh = model->GetMesh(meshRenderer.MeshIndex);
                    if (!mesh) continue;

                    if (auto ib = mesh->GetIndexBuffer())
                        m_VisibleTriCount += ib->GetCount() / 3;

                    u32 entityIdx = (u32)m_EntityLookup.size();
                    m_EntityLookup.push_back(entity);

                    DrawCommand dc;
                    dc.modelMatrix  = worldTransform.Matrix;
                    dc.materialSlot = 0;
                    dc.model        = model;
                    dc.meshIndex    = meshRenderer.MeshIndex;
                    dc.entityIndex  = entityIdx;

                    Material::RenderMode mode = Material::RenderMode::Opaque;
                    Material::CullMode cullMode = Material::CullMode::Back;

                    if (meshRenderer.MaterialUUID.IsValid())
                    {
                        auto material = AssetManager::GetAsset<Material>(meshRenderer.MaterialUUID);
                        if (material)
                        {
                            auto slotIt = m_MaterialSlotMap.find(material->Handle);
                            if (slotIt != m_MaterialSlotMap.end())
                                dc.materialSlot = slotIt->second;
                            mode = material->GetRenderMode();
                            cullMode = material->GetCullMode();
                        }
                    }
                    dc.cullMode = cullMode;

                    // Detect per-mesh skinning
                    if (meshRenderer.MeshIndex < model->GetMeshesData().size()
                        && model->GetMeshesData()[meshRenderer.MeshIndex].IsSkinned)
                    {
                        dc.isSkinned = true;
                        // Find Animation on this entity or parent
                        entt::entity animEntity = entt::null;
                        if (registry.any_of<Component::Animation>(entity))
                            animEntity = entity;
                        else if (registry.any_of<Component::Parent>(entity)) {
                            auto parentEnt = (entt::entity)registry.get<Component::Parent>(entity).m_Parent;
                            if (registry.valid(parentEnt) && registry.any_of<Component::Animation>(parentEnt))
                                animEntity = parentEnt;
                        }
                        if (animEntity != entt::null) {
                            auto& anim = registry.get<Component::Animation>(animEntity);
                            if (anim.BufferAllocated)
                                dc.boneOffset = anim.BoneBufferOffset;
                        }
                    }

                    switch (mode)
                    {
                        case Material::RenderMode::Cutout:      m_CutoutDraws.push_back(dc);      break;
                        case Material::RenderMode::Transparent:
                        case Material::RenderMode::Fade:        m_TransparentDraws.push_back(dc); break;
                        default:                                m_OpaqueDraws.push_back(dc);       break;
                    }
                }

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
                        // Rebind pipeline if cull mode or skinned state changed
                        if (dc.cullMode != currentCull || dc.isSkinned != currentSkinned)
                        {
                            currentCull = dc.cullMode;
                            currentSkinned = dc.isSkinned;

                            VKPipeline* newPipeline = nullptr;
                            if (currentSkinned)
                            {
                                newPipeline = m_GeoSkinnedPipelineManager.GetOrCreate(
                                    pbrUUID, mode, currentCull, polyMode, m_PBRSkinnedVertSpv, m_PBRFragSpv);
                            }
                            else
                            {
                                newPipeline = m_GeoPipelineManager.GetOrCreate(
                                    pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                            }
                            if (!newPipeline) continue;
                            newPipeline->Bind(cmd);
                        }

                        auto mesh = dc.model->GetMesh(dc.meshIndex);
                        auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                        auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                        if (!vb || !ib) continue;

                        ObjectPushConstants pc{};
                        pc.modelMatrix  = dc.modelMatrix;
                        pc.materialIndex = dc.materialSlot;
                        pc.shadeMode = static_cast<u32>(m_ShadeMode);
                        pc.entityID  = dc.entityIndex;
                        pc.boneOffset = dc.boneOffset;

                        vkCmdPushConstants(cmd, pipelineLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(ObjectPushConstants), &pc);

                        VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                        VkDeviceSize offsets[] = { 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                        vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);
                        vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);

                        // Capture for frame debugger
                        if (m_DebuggerState == DebuggerState::CaptureRequested)
                        {
                            std::string entName = "Entity";
                            if (dc.entityIndex < m_EntityLookup.size())
                            {
                                auto ent = m_EntityLookup[dc.entityIndex];
                                if (ent != entt::null && registry.valid(ent) && registry.any_of<Component::Tag>(ent))
                                    entName = registry.get<Component::Tag>(ent).m_Tag;
                            }
                            u32 vkCull = (currentCull == Material::CullMode::Back) ? VK_CULL_MODE_BACK_BIT
                                       : (currentCull == Material::CullMode::Front) ? VK_CULL_MODE_FRONT_BIT
                                       : VK_CULL_MODE_NONE;
                            CaptureDrawCall("GeometryPass",
                                dc.model->GetName() + "[" + std::to_string(dc.meshIndex) + "]",
                                entName, dc.entityIndex, ib->GetCount(), pc,
                                { "pbr", static_cast<u32>(mode), vkCull, polyMode, currentSkinned, true, true,
                                  mode == Material::RenderMode::Transparent || mode == Material::RenderMode::Fade });
                        }
                    }
                };

                DrawBatch(m_OpaqueDraws,       Material::RenderMode::Opaque);
                DrawBatch(m_CutoutDraws,      Material::RenderMode::Cutout);
                DrawBatch(m_TransparentDraws, Material::RenderMode::Transparent);

                EndCapturePass();
            }
        );
        return output;
    }

    RG::ResourceHandle RenderingSystem::AddSkyboxPass(
        RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth)
    {
        struct SkyboxPassData {
            RG::ResourceHandle colorTex;
            RG::ResourceHandle depthTex;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<SkyboxPassData>("SkyboxPass",
            [&](SkyboxPassData& data, RG::RenderPassBuilder& builder)
            {
                // Load existing scene color and depth from geometry pass
                data.colorTex = builder.Write(sceneColor,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
                data.depthTex = builder.WriteDepth(sceneDepth,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE);

                outputHandle = data.colorTex;
            },
            [this](SkyboxPassData& data, RG::RenderPassContext& ctx)
            {
                BeginCapturePass("SkyboxPass", "SceneColor", false,
                    { "skybox", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, false, false });

                if (!m_SkyboxPipeline || !m_SkyboxVB) { EndCapturePass(); return; }

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_SkyboxPipeline->Bind(cmd);

                // Bind all 5 descriptor sets (skybox only uses set 0, others required by layout)
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_GlobalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet,
                    BoneMatrixBuffer::GetDescriptorSet()
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_SkyboxPipeline->GetLayout(), 0, 5, sets, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.colorTex);
                VkViewport viewport{};
                viewport.width  = (float)res->desc.width;
                viewport.height = (float)res->desc.height;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                VkBuffer vb = m_SkyboxVB->GetVulkanBuffer();
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
                vkCmdDraw(cmd, 36, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                CaptureDrawCall("SkyboxPass", "SkyboxCube", "Skybox", 0, 0, dummyPC,
                    { "skybox", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, false, false });
                EndCapturePass();
            }
        );
        return outputHandle;
    }

    RG::ResourceHandle RenderingSystem::AddBloomPasses(RG::RenderGraph& rg, RG::ResourceHandle sceneColor)
    {
        if (!m_BloomExtractPipeline || !m_BloomBlurPipeline || !m_BloomA || !m_BloomB)
            return {}; // Post-process not initialized, skip bloom

        struct BloomPassData {
            RG::ResourceHandle output;
            RG::ResourceHandle input;
        };

        u32 halfW = m_BloomA->GetWidth();
        u32 halfH = m_BloomA->GetHeight();

        auto bloomAVk = std::static_pointer_cast<VKTexture>(m_BloomA);
        auto bloomBVk = std::static_pointer_cast<VKTexture>(m_BloomB);

        // --- Bloom Extract: SceneColor -> BloomA ---
        RG::ResourceHandle bloomAHandle;
        rg.AddPass<BloomPassData>("BloomExtract",
            [&](BloomPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "BloomA";
                desc.width  = halfW;
                desc.height = halfH;
                desc.format = RG::TextureFormat::RGBA16_Float;

                data.output = rg.ImportResource(desc,
                    (void*)bloomAVk->GetImage(), (void*)bloomAVk->GetImageView(),
                    RG::ResourceState::Undefined);
                data.output = builder.Write(data.output);

                data.input = builder.Read(sceneColor);
                bloomAHandle = data.output;
            },
            [this, halfW, halfH](BloomPassData& data, RG::RenderPassContext& ctx)
            {
                BeginCapturePass("BloomExtract", "BloomA", false,
                    { "bloomExtract", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_BloomExtractPipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_BloomExtractPipeline->GetLayout(), 0, 1, &m_BloomExtractDescSet, 0, nullptr);

                VkViewport vp{}; vp.width = (float)halfW; vp.height = (float)halfH; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { halfW, halfH };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                float pc[4] = { m_PostProcessSettings.bloomThreshold, 0, 0, 0 };
                vkCmdPushConstants(cmd, m_BloomExtractPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                CaptureDrawCall("BloomExtract", "FullscreenTriangle", "BloomExtract", 0, 0, dummyPC,
                    { "bloomExtract", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                EndCapturePass();
            }
        );

        // --- Bloom Blur Horizontal: BloomA -> BloomB ---
        RG::ResourceHandle bloomBHandle;
        rg.AddPass<BloomPassData>("BloomBlurH",
            [&](BloomPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "BloomB";
                desc.width  = halfW;
                desc.height = halfH;
                desc.format = RG::TextureFormat::RGBA16_Float;

                data.output = rg.ImportResource(desc,
                    (void*)bloomBVk->GetImage(), (void*)bloomBVk->GetImageView(),
                    RG::ResourceState::Undefined);
                data.output = builder.Write(data.output);

                data.input = builder.Read(bloomAHandle);
                bloomBHandle = data.output;
            },
            [this, halfW, halfH](BloomPassData& data, RG::RenderPassContext& ctx)
            {
                BeginCapturePass("BloomBlurH", "BloomB", false,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_BloomBlurPipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_BloomBlurPipeline->GetLayout(), 0, 1, &m_BloomBlurHDescSet, 0, nullptr);

                VkViewport vp{}; vp.width = (float)halfW; vp.height = (float)halfH; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { halfW, halfH };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                float pc[4] = { 1.0f / (float)halfW, 0.0f, 0.0f, 0.0f };
                vkCmdPushConstants(cmd, m_BloomBlurPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                CaptureDrawCall("BloomBlurH", "FullscreenTriangle", "BloomBlurH", 0, 0, dummyPC,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                EndCapturePass();
            }
        );

        // --- Bloom Blur Vertical: BloomB -> BloomA ---
        RG::ResourceHandle finalBloomHandle;
        rg.AddPass<BloomPassData>("BloomBlurV",
            [&](BloomPassData& data, RG::RenderPassBuilder& builder)
            {
                // Re-import BloomA with a new identity for the second write
                RG::TextureDesc desc;
                desc.name   = "BloomAFinal";
                desc.width  = halfW;
                desc.height = halfH;
                desc.format = RG::TextureFormat::RGBA16_Float;

                data.output = rg.ImportResource(desc,
                    (void*)bloomAVk->GetImage(), (void*)bloomAVk->GetImageView(),
                    RG::ResourceState::Undefined);
                data.output = builder.Write(data.output);

                data.input = builder.Read(bloomBHandle);
                finalBloomHandle = data.output;
            },
            [this, halfW, halfH](BloomPassData& data, RG::RenderPassContext& ctx)
            {
                BeginCapturePass("BloomBlurV", "BloomAFinal", false,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_BloomBlurPipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_BloomBlurPipeline->GetLayout(), 0, 1, &m_BloomBlurVDescSet, 0, nullptr);

                VkViewport vp{}; vp.width = (float)halfW; vp.height = (float)halfH; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { halfW, halfH };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                float pc[4] = { 0.0f, 1.0f / (float)halfH, 0.0f, 0.0f };
                vkCmdPushConstants(cmd, m_BloomBlurPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                CaptureDrawCall("BloomBlurV", "FullscreenTriangle", "BloomBlurV", 0, 0, dummyPC,
                    { "bloomBlur", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                EndCapturePass();
            }
        );

        return finalBloomHandle;
    }

    RG::ResourceHandle RenderingSystem::AddPostProcessPass(
        RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle bloomResult)
    {
        if (!m_PostProcessPipeline || !m_LDROutput)
            return sceneColor; // Fallback: pass HDR scene color through

        struct PostProcessPassData {
            RG::ResourceHandle output;
            RG::ResourceHandle hdrInput;
            RG::ResourceHandle bloomInput;
        };

        RG::ResourceHandle outputHandle;
        auto ldrVk = std::static_pointer_cast<VKTexture>(m_LDROutput);

        rg.AddPass<PostProcessPassData>("PostProcess",
            [&](PostProcessPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "LDROutput";
                desc.width  = m_LDROutput->GetWidth();
                desc.height = m_LDROutput->GetHeight();
                desc.format = RG::TextureFormat::RGBA8_Unorm;

                data.output = rg.ImportResource(desc,
                    (void*)ldrVk->GetImage(), (void*)ldrVk->GetImageView(),
                    RG::ResourceState::ShaderResource);
                data.output = builder.Write(data.output);

                data.hdrInput = builder.Read(sceneColor);
                if (bloomResult.IsValid())
                    data.bloomInput = builder.Read(bloomResult);

                outputHandle = data.output;
            },
            [this](PostProcessPassData& data, RG::RenderPassContext& ctx)
            {
                BeginCapturePass("PostProcess", "LDROutput", false,
                    { "postprocess", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_PostProcessPipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_PostProcessPipeline->GetLayout(), 0, 1, &m_CompositeDescSet, 0, nullptr);

                u32 w = m_LDROutput->GetWidth();
                u32 h = m_LDROutput->GetHeight();

                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                CaptureDrawCall("PostProcess", "FullscreenTriangle", "PostProcess", 0, 0, dummyPC,
                    { "postprocess", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, false });
                EndCapturePass();
            }
        );

        return outputHandle;
    }

    void RenderingSystem::CollectSelectedHandles(const std::vector<Entity>& selected, std::unordered_set<entt::entity>& outHandles) const
    {
        for (const auto& entity : selected)
        {
            if (!entity || !entity.IsValid()) continue;
            outHandles.insert((entt::entity)entity);
            // Recursively include children so outline wraps entire subtrees
            for (const auto& child : entity.GetChildren())
            {
                std::vector<Entity> childVec = { child };
                CollectSelectedHandles(childVec, outHandles);
            }
        }
    }

    SelectionMaskOutput RenderingSystem::AddSelectionMaskPass(RG::RenderGraph& rg, entt::registry& registry)
    {
        struct SelectionMaskPassData {
            RG::ResourceHandle maskTex;
            RG::ResourceHandle depthTex;
        };

        SelectionMaskOutput output;

        rg.AddPass<SelectionMaskPassData>("SelectionMaskPass",
            [&](SelectionMaskPassData& data, RG::RenderPassBuilder& builder)
            {
                // Import selection mask (RGBA8)
                auto vkMask = std::static_pointer_cast<VKTexture>(m_SelectionMask);
                RG::TextureDesc maskDesc;
                maskDesc.name   = "SelectionMask";
                maskDesc.width  = m_SelectionMask->GetWidth();
                maskDesc.height = m_SelectionMask->GetHeight();
                maskDesc.format = RG::TextureFormat::RGBA8_Unorm;

                data.maskTex = rg.ImportResource(maskDesc,
                    (void*)vkMask->GetImage(), (void*)vkMask->GetImageView(),
                    RG::ResourceState::Undefined);

                VkClearValue colorClear{};
                colorClear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
                data.maskTex = builder.Write(data.maskTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, colorClear);

                // Import selection depth (D32_Float)
                auto vkDepth = std::static_pointer_cast<VKTexture>(m_SelectionDepth);
                RG::TextureDesc depthDesc;
                depthDesc.name   = "SelectionDepth";
                depthDesc.width  = m_SelectionDepth->GetWidth();
                depthDesc.height = m_SelectionDepth->GetHeight();
                depthDesc.format = RG::TextureFormat::D32_Float;

                data.depthTex = rg.ImportResource(depthDesc,
                    (void*)vkDepth->GetImage(), (void*)vkDepth->GetImageView(),
                    RG::ResourceState::Undefined);

                VkClearValue depthClear{};
                depthClear.depthStencil = { 1.0f, 0 };
                data.depthTex = builder.WriteDepth(data.depthTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depthClear);

                output.mask  = data.maskTex;
                output.depth = data.depthTex;
            },
            [this, &registry](SelectionMaskPassData& data, RG::RenderPassContext& ctx)
            {
                BeginCapturePass("SelectionMaskPass", "SelectionMask", false,
                    { "selectionMask", 0, VK_CULL_MODE_BACK_BIT, VK_POLYGON_MODE_FILL, false, true, true, false });

                if (!m_SelectionMaskPipeline) { EndCapturePass(); return; }

                // Build set of selected entity handles (including descendants)
                std::unordered_set<entt::entity> selectedSet;
                CollectSelectedHandles(EditorSelection::GetSelectedEntities(), selectedSet);
                if (selectedSet.empty()) return;

                VkCommandBuffer cmd = ctx.commandBuffer;

                // Bind descriptor sets (same 5 sets as geometry/shadow passes)
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_GlobalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet,
                    BoneMatrixBuffer::GetDescriptorSet()
                };

                m_SelectionMaskPipeline->Bind(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_SelectionMaskPipeline->GetLayout(), 0, 5, sets, 0, nullptr);

                u32 w = m_SelectionMask->GetWidth();
                u32 h = m_SelectionMask->GetHeight();
                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                bool currentSkinned = false;

                auto view = registry.view<WorldTransform, MeshRenderer>();
                for (auto [entity, worldTransform, meshRenderer] : view.each())
                {
                    // Only draw selected entities
                    if (selectedSet.find(entity) == selectedSet.end()) continue;

                    auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID);
                    if (!model) continue;
                    auto mesh = model->GetMesh(meshRenderer.MeshIndex);
                    if (!mesh) continue;

                    auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                    auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                    if (!vb || !ib) continue;

                    // Check per-mesh skinning
                    bool isSkinned = false;
                    u32 boneOffset = 0;
                    if (meshRenderer.MeshIndex < model->GetMeshesData().size())
                        isSkinned = model->GetMeshesData()[meshRenderer.MeshIndex].IsSkinned;

                    if (isSkinned)
                    {
                        entt::entity animEntity = entt::null;
                        if (registry.any_of<Component::Animation>(entity))
                            animEntity = entity;
                        else if (registry.any_of<Component::Parent>(entity)) {
                            auto parentEnt = (entt::entity)registry.get<Component::Parent>(entity).m_Parent;
                            if (registry.valid(parentEnt) && registry.any_of<Component::Animation>(parentEnt))
                                animEntity = parentEnt;
                        }
                        if (animEntity != entt::null) {
                            auto& anim = registry.get<Component::Animation>(animEntity);
                            if (anim.BufferAllocated)
                                boneOffset = anim.BoneBufferOffset;
                        }
                    }

                    // Switch pipeline if skinned state changed
                    if (isSkinned != currentSkinned)
                    {
                        currentSkinned = isSkinned;
                        if (isSkinned && m_SelectionMaskSkinnedPipeline)
                        {
                            m_SelectionMaskSkinnedPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_SelectionMaskSkinnedPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                        }
                        else
                        {
                            m_SelectionMaskPipeline->Bind(cmd);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_SelectionMaskPipeline->GetLayout(), 0, 5, sets, 0, nullptr);
                        }
                    }

                    VkPipelineLayout activeLayout = (currentSkinned && m_SelectionMaskSkinnedPipeline)
                        ? m_SelectionMaskSkinnedPipeline->GetLayout()
                        : m_SelectionMaskPipeline->GetLayout();

                    ObjectPushConstants pc{};
                    pc.modelMatrix   = worldTransform.Matrix;
                    pc.materialIndex = 0;
                    pc.boneOffset    = boneOffset;

                    vkCmdPushConstants(cmd, activeLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(ObjectPushConstants), &pc);

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);
                }

                EndCapturePass();
            }
        );

        return output;
    }

    RG::ResourceHandle RenderingSystem::AddOutlinePass(
        RG::RenderGraph& rg, RG::ResourceHandle ldrOutput, SelectionMaskOutput maskOutput, RG::ResourceHandle sceneDepth)
    {
        if (!m_OutlinePipeline || !m_LDROutput)
            return ldrOutput;

        struct OutlinePassData {
            RG::ResourceHandle output;
            RG::ResourceHandle maskInput;
            RG::ResourceHandle selDepthInput;
            RG::ResourceHandle scnDepthInput;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<OutlinePassData>("OutlinePass",
            [&](OutlinePassData& data, RG::RenderPassBuilder& builder)
            {
                // Write to LDR output (alpha-blend outline on top)
                data.output = builder.Write(ldrOutput,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);

                // Read selection mask, selection depth, and scene depth
                data.maskInput     = builder.Read(maskOutput.mask);
                data.selDepthInput = builder.Read(maskOutput.depth);
                data.scnDepthInput = builder.Read(sceneDepth);

                outputHandle = data.output;
            },
            [this](OutlinePassData& data, RG::RenderPassContext& ctx)
            {
                BeginCapturePass("OutlinePass", "LDROutput", false,
                    { "outline", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_OutlinePipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_OutlinePipeline->GetLayout(), 0, 1, &m_OutlineDescSet, 0, nullptr);

                u32 w = m_LDROutput->GetWidth();
                u32 h = m_LDROutput->GetHeight();

                VkViewport vp{}; vp.width = (float)w; vp.height = (float)h; vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { w, h };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                // Push constants: outlineWidth, texelSize, outlineColor, occludedAlpha
                struct OutlinePushConstants {
                    float outlineWidth;
                    float texelSizeX;
                    float texelSizeY;
                    float outlineColorR;
                    float outlineColorG;
                    float outlineColorB;
                    float outlineColorA;
                    float occludedAlpha;
                } pc;

                pc.outlineWidth     = 1.5f;
                pc.texelSizeX       = 1.0f / (float)w;
                pc.texelSizeY       = 1.0f / (float)h;
                pc.outlineColorR    = m_OutlineColor.r;
                pc.outlineColorG    = m_OutlineColor.g;
                pc.outlineColorB    = m_OutlineColor.b;
                pc.outlineColorA    = m_OutlineColor.a;
                pc.occludedAlpha    = 0.65f;

                vkCmdPushConstants(cmd, m_OutlinePipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                CaptureDrawCall("OutlinePass", "FullscreenTriangle", "OutlinePass", 0, 0, dummyPC,
                    { "outline", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                EndCapturePass();
            }
        );

        return outputHandle;
    }

    RG::ResourceHandle RenderingSystem::AddGridPass(
        RG::RenderGraph& rg, RG::ResourceHandle sceneColor, RG::ResourceHandle sceneDepth)
    {
        if (!m_GridPipeline)
            return sceneColor;

        struct GridPassData {
            RG::ResourceHandle colorTex;
            RG::ResourceHandle depthInput;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<GridPassData>("GridPass",
            [&](GridPassData& data, RG::RenderPassBuilder& builder)
            {
                // Load existing scene color and alpha-blend grid on top
                data.colorTex = builder.Write(sceneColor,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);

                // Scene depth as shader resource (not attachment)
                data.depthInput = builder.Read(sceneDepth);

                outputHandle = data.colorTex;
            },
            [this](GridPassData& data, RG::RenderPassContext& ctx)
            {
                BeginCapturePass("GridPass", "SceneColor", false,
                    { "grid", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_GridPipeline->Bind(cmd);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_GridPipeline->GetLayout(), 0, 1, &m_GridDescSet, 0, nullptr);

                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.colorTex);
                VkViewport vp{};
                vp.width  = (float)res->desc.width;
                vp.height = (float)res->desc.height;
                vp.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &vp);
                VkRect2D sc{}; sc.extent = { res->desc.width, res->desc.height };
                vkCmdSetScissor(cmd, 0, 1, &sc);

                // Must match GridPushConstants in grid.frag (16 floats / 64 bytes).
                struct GridPushConstants {
                    float axisXColor[4];
                    float axisZColor[4];
                    float gridColor[4];
                    float majorScale;
                    float fadeStart;
                    float fadeEnd;
                    float lineThickness;
                } gpc{};

                // Axis colors mirror EditorColors::AxisX/AxisZ (engine cannot depend on the editor lib).
                gpc.axisXColor[0] = 0.80f; gpc.axisXColor[1] = 0.10f; gpc.axisXColor[2] = 0.15f; gpc.axisXColor[3] = 1.00f;
                gpc.axisZColor[0] = 0.10f; gpc.axisZColor[1] = 0.25f; gpc.axisZColor[2] = 0.80f; gpc.axisZColor[3] = 1.00f;
                gpc.gridColor[0]  = 0.41f; gpc.gridColor[1]  = 0.41f; gpc.gridColor[2]  = 0.41f; gpc.gridColor[3]  = 0.50f;
                gpc.majorScale    = 1.0f;
                gpc.fadeStart     = 20.0f;
                gpc.fadeEnd       = 200.0f;
                gpc.lineThickness = 1.00f;

                vkCmdPushConstants(cmd, m_GridPipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gpc), &gpc);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                ObjectPushConstants dummyPC{};
                CaptureDrawCall("GridPass", "FullscreenTriangle", "GridPass", 0, 0, dummyPC,
                    { "grid", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                EndCapturePass();
            }
        );

        return outputHandle;
    }

    void RenderingSystem::AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor)
    {
        struct ImGuiPassData {
            RG::ResourceHandle backbuffer;
            RG::ResourceHandle sceneTexture;
        };

        rg.AddPass<ImGuiPassData>("ImGuiPass",
            [&](ImGuiPassData& data, RG::RenderPassBuilder& builder)
            {
                auto* vkRenderer = static_cast<VulkanBackend*>(Renderer::GetBackend());
                VkImage     swapchainImage = vkRenderer->GetSwapchain().GetImage(vkRenderer->GetSwapchain().GetCurrentFrameIndex());
                VkImageView swapchainView  = vkRenderer->GetSwapchain().GetImageView(vkRenderer->GetSwapchain().GetCurrentFrameIndex());

                RG::TextureDesc desc;
                desc.name   = "Backbuffer";
                desc.width  = vkRenderer->GetSwapchain().GetExtent().width;
                desc.height = vkRenderer->GetSwapchain().GetExtent().height;
                desc.format = RG::TextureFormat::BGRA8_Unorm;

                data.backbuffer = rg.ImportResource(desc,
                    (void*)swapchainImage, (void*)swapchainView,
                    RG::ResourceState::Undefined);

                data.backbuffer = builder.Write(data.backbuffer);

                if (sceneColor.IsValid())
                    data.sceneTexture = builder.Read(sceneColor);
            },
            [this](ImGuiPassData& data, RG::RenderPassContext& ctx)
            {
                BeginCapturePass("ImGuiPass", "Backbuffer", false,
                    { "imgui", 0, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, false, false, false, true });
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), ctx.commandBuffer);
                EndCapturePass();
            }
        );
    }

    // =========================================================================
    // Frame Debugger helpers
    // =========================================================================
    //  Frame Debugger Capture Helpers
    // =========================================================================

    void RenderingSystem::BeginCapturePass(const std::string& name, const std::string& activeTarget,
                                           bool isDepth, const RG::CapturedPipelineState& ps)
    {
        if (m_DebuggerState != DebuggerState::CaptureRequested) return;

        RG::CapturedPass cp;
        cp.name              = name;
        cp.firstDrawIndex    = (u32)m_CapturedFrame.drawCalls.size();
        cp.drawCallCount     = 0;
        cp.pipelineState     = ps;
        cp.activeRenderTarget = activeTarget;
        cp.isDepthTarget     = isDepth;
        m_CapturedFrame.passes.push_back(std::move(cp));
    }

    void RenderingSystem::EndCapturePass()
    {
        if (m_DebuggerState != DebuggerState::CaptureRequested) return;
        if (m_CapturedFrame.passes.empty()) return;

        auto& cp = m_CapturedFrame.passes.back();
        cp.drawCallCount = (u32)m_CapturedFrame.drawCalls.size() - cp.firstDrawIndex;
    }

    void RenderingSystem::CaptureDrawCall(const std::string& passName, const std::string& meshName,
                                          const std::string& entityName, u32 entityIndex, u32 indexCount,
                                          const ObjectPushConstants& pc, const RG::CapturedPipelineState& ps)
    {
        if (m_DebuggerState != DebuggerState::CaptureRequested) return;

        RG::CapturedDrawCall cdc;
        cdc.globalIndex    = (u32)m_CapturedFrame.drawCalls.size();
        cdc.passIndex      = m_CapturedFrame.passes.empty() ? 0 : (u32)(m_CapturedFrame.passes.size() - 1);
        cdc.passLocalIndex = m_CapturedFrame.passes.empty() ? 0
                           : (u32)(m_CapturedFrame.drawCalls.size() - m_CapturedFrame.passes.back().firstDrawIndex);
        cdc.passName       = passName;
        cdc.meshName       = meshName;
        cdc.entityName     = entityName;
        cdc.entityIndex    = entityIndex;
        cdc.indexCount     = indexCount;
        cdc.modelMatrix    = pc.modelMatrix;
        cdc.materialIndex  = pc.materialIndex;
        cdc.shadeMode      = pc.shadeMode;
        cdc.entityID       = pc.entityID;
        cdc.boneOffset     = pc.boneOffset;
        cdc.pipelineState  = ps;
        m_CapturedFrame.drawCalls.push_back(std::move(cdc));
    }

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
        if (m_DebugBlitPipeline) return; // Already initialized

        auto shadersPath = FileSystem::EngineAssetsPath("shaders");
        m_DebugBlitFragSpv  = ShaderCompiler::Compile(shadersPath / "debugBlit.frag");
        m_DebugDepthFragSpv = ShaderCompiler::Compile(shadersPath / "debugDepth.frag");

        if (m_DebugBlitFragSpv.empty() || m_DebugDepthFragSpv.empty() || m_FullscreenVertSpv.empty())
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
        vkCreateSampler(device, &samplerCI, nullptr, &m_DebugBlitSampler);

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
        vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &m_DebugBlitDescSetLayout);

        // Descriptor pool
        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo poolCI{};
        poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolCI.maxSets       = 1;
        poolCI.poolSizeCount = 1;
        poolCI.pPoolSizes    = &poolSize;
        poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &poolCI, nullptr, &m_DebugBlitDescPool);

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo allocCI{};
        allocCI.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocCI.descriptorPool     = m_DebugBlitDescPool;
        allocCI.descriptorSetCount = 1;
        allocCI.pSetLayouts        = &m_DebugBlitDescSetLayout;
        vkAllocateDescriptorSets(device, &allocCI, &m_DebugBlitDescSet);

        // Create blit pipeline (color)
        std::vector<VkDescriptorSetLayout> layouts = { m_DebugBlitDescSetLayout };
        PipelineConfig blitConfig;
        blitConfig.depthTest  = false;
        blitConfig.depthWrite = false;
        blitConfig.cullMode         = VK_CULL_MODE_NONE;
        blitConfig.colorFormats     = { VK_FORMAT_R8G8B8A8_UNORM };
        m_DebugBlitPipeline = std::make_unique<VKPipeline>(
            blitConfig, m_FullscreenVertSpv, m_DebugBlitFragSpv, layouts);

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

        m_DebugDepthPipeline = std::make_unique<VKPipeline>(
            depthConfig, m_FullscreenVertSpv, m_DebugDepthFragSpv, layouts);
    }

    RG::ResourceHandle RenderingSystem::AddDebugBlitPass(RG::RenderGraph& rg, RG::ResourceHandle inputHandle, bool isDepth)
    {
        if (!m_DebugBlitPipeline || !m_LDROutput) return inputHandle;

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

                if (isDepth && m_DebugDepthPipeline)
                {
                    m_DebugDepthPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_DebugDepthPipeline->GetLayout(), 0, 1, &m_DebugBlitDescSet, 0, nullptr);

                    float pc[2] = { 0.1f, 200.0f }; // near/far for shadow maps
                    vkCmdPushConstants(cmd, m_DebugDepthPipeline->GetLayout(),
                        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
                }
                else
                {
                    m_DebugBlitPipeline->Bind(cmd);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        m_DebugBlitPipeline->GetLayout(), 0, 1, &m_DebugBlitDescSet, 0, nullptr);
                }

                vkCmdDraw(cmd, 3, 1, 0, 0);
            }
        );

        return outputHandle;
    }

    void RenderingSystem::RenderCapturedFrame(u32 maxDrawCalls, Scene* scene)
    {
        if (!m_CapturedFrame.valid) return;

        // Ensure debug blit resources are available
        InitDebugBlitResources();

        auto& registry = scene->Registry();
        m_ReplayDrawCounter = 0;

        RG::RenderGraph rg(*m_FrameAllocator);

        // Determine which pass the draw limit falls in, to know what render target is active
        std::string activeTarget = "SceneColor";
        bool isDepthTarget       = false;
        bool postProcessReached  = false;

        for (auto& cp : m_CapturedFrame.passes)
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
                        if (m_ReplayDrawCounter >= maxDrawCalls) return;

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
                        m_ReplayDrawCounter++;
                    }
                };

                ReplayShadowDraws(m_CapturedOpaqueDraws);
                ReplayShadowDraws(m_CapturedCutoutDraws);
                ReplayShadowDraws(m_CapturedTransparentDraws);
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
                    VkDescriptorSet sets[] = { m_GlobalDescriptorSet, bindlessSet, MaterialSystem::GetDescriptorSet(), m_LightDescSet, BoneMatrixBuffer::GetDescriptorSet() };
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 5, sets, 0, nullptr);

                    RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.outputTex);
                    VkViewport viewport{}; viewport.width = (float)res->desc.width; viewport.height = (float)res->desc.height; viewport.maxDepth = 1.0f;
                    vkCmdSetViewport(cmd, 0, 1, &viewport);
                    VkRect2D scissor{}; scissor.extent = { res->desc.width, res->desc.height };
                    vkCmdSetScissor(cmd, 0, 1, &scissor);

                    auto DrawBatchReplay = [&](const std::vector<DrawCommand>& draws, Material::RenderMode mode)
                    {
                        if (draws.empty() || m_ReplayDrawCounter >= maxDrawCalls) return;

                        Material::CullMode currentCull = Material::CullMode::Back;
                        bool currentSkinned = false;
                        auto* pipeline = m_GeoPipelineManager.GetOrCreate(pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                        if (!pipeline) return;
                        pipeline->Bind(cmd);

                        for (const auto& dc : draws)
                        {
                            if (m_ReplayDrawCounter >= maxDrawCalls) return;

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

                            ObjectPushConstants pc{};
                            pc.modelMatrix   = dc.modelMatrix;
                            pc.materialIndex = dc.materialSlot;
                            pc.shadeMode     = static_cast<u32>(m_ShadeMode);
                            pc.entityID      = dc.entityIndex;
                            pc.boneOffset    = dc.boneOffset;
                            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof(ObjectPushConstants), &pc);

                            VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                            VkDeviceSize offsets[] = { 0 };
                            vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                            vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);
                            vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);
                            m_ReplayDrawCounter++;
                        }
                    };

                    DrawBatchReplay(m_CapturedOpaqueDraws,      Material::RenderMode::Opaque);
                    DrawBatchReplay(m_CapturedCutoutDraws,      Material::RenderMode::Cutout);
                    DrawBatchReplay(m_CapturedTransparentDraws, Material::RenderMode::Transparent);
                }
            );
        }

        // --- Skybox Pass (replay) ---
        RG::ResourceHandle skyboxColor = geoOutput.color;
        if (m_SkyboxPipeline && m_SkyboxVB && m_ReplayDrawCounter < maxDrawCalls)
        {
            struct SkyboxReplayData { RG::ResourceHandle colorTex, depthTex; };
            rg.AddPass<SkyboxReplayData>("SkyboxPass",
                [&](SkyboxReplayData& data, RG::RenderPassBuilder& builder) {
                    data.colorTex = builder.Write(geoOutput.color, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
                    data.depthTex = builder.WriteDepth(geoOutput.depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE);
                    skyboxColor = data.colorTex;
                },
                [this, maxDrawCalls](SkyboxReplayData& data, RG::RenderPassContext& ctx) {
                    if (m_ReplayDrawCounter >= maxDrawCalls) return;
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
                    m_ReplayDrawCounter++;
                }
            );
        }

        // Determine final output handle for ImGui dependency
        RG::ResourceHandle finalOutput = skyboxColor;

        // --- Fullscreen passes (bloom, postprocess) - only if draw counter allows ---
        bool ppReached = false;
        if (m_ReplayDrawCounter < maxDrawCalls && m_BloomExtractPipeline && m_BloomBlurPipeline && m_BloomA && m_BloomB)
        {
            // Simplified: add bloom + postprocess as single draws each
            RG::ResourceHandle bloomResult = AddBloomPasses(rg, skyboxColor);
            if (m_ReplayDrawCounter + 3 < maxDrawCalls) // 3 bloom passes
            {
                m_ReplayDrawCounter += 3;
                if (m_PostProcessPipeline && m_LDROutput && m_ReplayDrawCounter < maxDrawCalls)
                {
                    finalOutput = AddPostProcessPass(rg, skyboxColor, bloomResult);
                    m_ReplayDrawCounter++;
                    ppReached = true;
                }
            }
        }

        // --- Rescue Blit: if we stopped before PostProcess ---
        if (!ppReached && m_DebugBlitPipeline)
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
                imgInfo.sampler     = m_DebugBlitSampler;
                imgInfo.imageView   = vkTex->GetImageView();
                imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet write{};
                write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet          = m_DebugBlitDescSet;
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
