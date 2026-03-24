#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/Profiler.h"
#include "luth/scene/Scene.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/MaterialSystem.h"
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
            m_SceneColor    = Texture::Create(viewportWidth, viewportHeight, TextureFormat::RGBA16F);
            m_SceneDepth    = Texture::Create(viewportWidth, viewportHeight, TextureFormat::D32_Float);
            m_LDROutput     = Texture::Create(viewportWidth, viewportHeight, TextureFormat::RGBA8);
            m_EntityIDBuffer = Texture::Create(viewportWidth, viewportHeight, TextureFormat::R32_Uint);

            InitGlobalUniforms();
            InitShadowResources();

            // Load shaders via AssetManager (imports + caches SPIR-V on first run)
            UUID pbrUUID = AssetDatabase::GetUUID(FileSystem::AssetsPath() / "shaders/pbr.vert");
            auto pbrShader = std::static_pointer_cast<Shader>(AssetManager::LoadImmediate(pbrUUID));
            if (!pbrShader)
            {
                LH_CORE_ERROR("Failed to load PBR shader via AssetManager!");
                return;
            }
            ShaderLibrary::Register("pbr", pbrShader);

            UUID shadowUUID = AssetDatabase::GetUUID(FileSystem::AssetsPath() / "shaders/shadowDepth.vert");
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

            // Load post-process shaders via ShaderCompiler (not asset pipeline — inline shaders)
            {
                auto shadersPath = FileSystem::AssetsPath() / "shaders";
                m_FullscreenVertSpv    = ShaderCompiler::Compile(shadersPath / "fullscreen.vert");
                m_BloomExtractFragSpv  = ShaderCompiler::Compile(shadersPath / "bloomExtract.frag");
                m_BloomBlurFragSpv     = ShaderCompiler::Compile(shadersPath / "bloomBlur.frag");
                m_PostProcessFragSpv   = ShaderCompiler::Compile(shadersPath / "postprocess.frag");
                m_OutlineFragSpv       = ShaderCompiler::Compile(shadersPath / "outline.frag");

                if (m_FullscreenVertSpv.empty() || m_BloomExtractFragSpv.empty() ||
                    m_BloomBlurFragSpv.empty() || m_PostProcessFragSpv.empty() ||
                    m_OutlineFragSpv.empty())
                {
                    LH_CORE_ERROR("Failed to compile post-process shaders!");
                }
            }

            InitPostProcessResources();
            InitIBLResources();
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
                } else {
                    m_GeoPipelineManager.Clear();
                }
                m_ShadowPipeline.reset();
                m_SkyboxPipeline.reset();
                m_BloomExtractPipeline.reset();
                m_BloomBlurPipeline.reset();
                m_PostProcessPipeline.reset();
                m_OutlinePipeline.reset();
                CreatePipelines();
                LH_CORE_INFO("Pipelines rebuilt after shader reload: {}", name);
            });

            // File watcher for shader hot-reload
            m_ShaderWatcher.AddWatch(FileSystem::AssetsPath() / "shaders");
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

        VkDevice device = VulkanContext::Get().GetDevice();

        if (m_OutlineSampler)
            vkDestroySampler(device, m_OutlineSampler, nullptr);
        if (m_OutlineDescSetLayout)
            vkDestroyDescriptorSetLayout(device, m_OutlineDescSetLayout, nullptr);
        if (m_OutlineDescPool)
            vkDestroyDescriptorPool(device, m_OutlineDescPool, nullptr);
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
            // Nearest-neighbor sampler for integer entity ID texture
            VkSamplerCreateInfo outlineSamplerInfo{};
            outlineSamplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            outlineSamplerInfo.magFilter = VK_FILTER_NEAREST;
            outlineSamplerInfo.minFilter = VK_FILTER_NEAREST;
            outlineSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            outlineSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            vkCreateSampler(device, &outlineSamplerInfo, nullptr, &m_OutlineSampler);

            // Descriptor set layout: binding 0 = usampler2D (entity ID)
            VkDescriptorSetLayoutBinding outlineBinding{};
            outlineBinding.binding = 0;
            outlineBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            outlineBinding.descriptorCount = 1;
            outlineBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo outlineLayoutInfo{};
            outlineLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            outlineLayoutInfo.bindingCount = 1;
            outlineLayoutInfo.pBindings = &outlineBinding;
            vkCreateDescriptorSetLayout(device, &outlineLayoutInfo, nullptr, &m_OutlineDescSetLayout);

            // Descriptor pool: 1 set, 1 sampler
            VkDescriptorPoolSize outlinePoolSize{};
            outlinePoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            outlinePoolSize.descriptorCount = 1;

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

            // Write descriptor: entity ID buffer
            auto vkEntityID = std::static_pointer_cast<VKTexture>(m_EntityIDBuffer);
            VkDescriptorImageInfo entityIDImgInfo{};
            entityIDImgInfo.sampler     = m_OutlineSampler;
            entityIDImgInfo.imageView   = vkEntityID->GetImageView();
            entityIDImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet outlineWrite{};
            outlineWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            outlineWrite.dstSet = m_OutlineDescSet;
            outlineWrite.dstBinding = 0;
            outlineWrite.descriptorCount = 1;
            outlineWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            outlineWrite.pImageInfo = &entityIDImgInfo;
            vkUpdateDescriptorSets(device, 1, &outlineWrite, 0, nullptr);
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

    void RenderingSystem::InitIBLResources()
    {
        VkDevice device = VulkanContext::Get().GetDevice();
        auto shadersPath = FileSystem::AssetsPath() / "shaders";

        // ---- 1. Load HDR environment map ----
        fs::path hdrPath = FileSystem::AssetsPath() / "textures" / "environment.hdr";
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

        // 4-set layout shared by all pipelines
        std::vector<VkDescriptorSetLayout> layouts = {
            m_GlobalSetLayout,
            VulkanContext::Get().GetBindlessSet().GetLayout(),
            MaterialSystem::GetDescriptorSetLayout(),
            m_LightSetLayout
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
            outlinePC.size = sizeof(float) * 8; // selectedEntityID(u32) + outlineWidth(f32) + texelSize(vec2) + outlineColor(vec4) = 32 bytes

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
            RG::ResourceHandle skyboxColor = AddSkyboxPass(rg, geoOutput.color, geoOutput.depth);
            RG::ResourceHandle bloomResult = AddBloomPasses(rg, skyboxColor);
            RG::ResourceHandle ldrOutput   = AddPostProcessPass(rg, skyboxColor, bloomResult);
            RG::ResourceHandle finalOutput = AddOutlinePass(rg, ldrOutput, geoOutput.entityID);
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

                if (!m_ShadowPipeline) { LH_CORE_ERROR("Shadow pipeline is null!"); return; }
                m_ShadowPipeline->Bind(cmd);

                // Bind all 4 sets (sets 1-3 are unused in shadow pass but layout requires them)
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_GlobalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_ShadowPipeline->GetLayout(), 0, 4, sets, 0, nullptr);

                // Shadow map viewport
                VkViewport viewport{};
                viewport.width    = 2048.0f;
                viewport.height   = 2048.0f;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.extent = { 2048, 2048 };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

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

                    ObjectPushConstants pc{};
                    pc.modelMatrix   = worldTransform.Matrix;
                    pc.materialIndex = 0;

                    vkCmdPushConstants(cmd, m_ShadowPipeline->GetLayout(),
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(ObjectPushConstants), &pc);

                    VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                    VkDeviceSize offsets[] = { 0 };
                    vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                    vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);
                }
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

                UUID pbrUUID = ShaderLibrary::Get("pbr")->Handle;
                VkPolygonMode polyMode = (m_ShadeMode == ShadeMode::Wireframe) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
                auto* opaquePipeline = m_GeoPipelineManager.GetOrCreate(
                    pbrUUID, Material::RenderMode::Opaque, Material::CullMode::Back, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                if (!opaquePipeline) return;
                VkPipelineLayout pipelineLayout = opaquePipeline->GetLayout();

                // Bind all 4 descriptor sets
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_GlobalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout, 0, 4, sets, 0, nullptr);

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
                    auto* pipeline = m_GeoPipelineManager.GetOrCreate(
                        pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv, m_PBRFragSpv);
                    if (!pipeline) return;

                    pipeline->Bind(cmd);

                    for (const auto& dc : draws)
                    {
                        // Rebind pipeline if cull mode changed
                        if (dc.cullMode != currentCull)
                        {
                            currentCull = dc.cullMode;
                            auto* newPipeline = m_GeoPipelineManager.GetOrCreate(
                                pbrUUID, mode, currentCull, polyMode, m_PBRVertSpv, m_PBRFragSpv);
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

                        vkCmdPushConstants(cmd, pipelineLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(ObjectPushConstants), &pc);

                        VkBuffer vbuf[] = { vb->GetVulkanBuffer() };
                        VkDeviceSize offsets[] = { 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, offsets);
                        vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);
                        vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);
                    }
                };

                DrawBatch(m_OpaqueDraws,       Material::RenderMode::Opaque);
                DrawBatch(m_CutoutDraws,      Material::RenderMode::Cutout);
                DrawBatch(m_TransparentDraws, Material::RenderMode::Transparent);
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
                if (!m_SkyboxPipeline || !m_SkyboxVB) return;

                VkCommandBuffer cmd = ctx.commandBuffer;
                m_SkyboxPipeline->Bind(cmd);

                // Bind all 4 descriptor sets (skybox only uses set 0)
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet sets[] = {
                    m_GlobalDescriptorSet,
                    bindlessSet,
                    MaterialSystem::GetDescriptorSet(),
                    m_LightDescSet
                };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_SkyboxPipeline->GetLayout(), 0, 4, sets, 0, nullptr);

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
            }
        );

        return outputHandle;
    }

    RG::ResourceHandle RenderingSystem::AddOutlinePass(
        RG::RenderGraph& rg, RG::ResourceHandle ldrOutput, RG::ResourceHandle entityIDHandle)
    {
        if (!m_OutlinePipeline || !m_LDROutput)
            return ldrOutput;

        struct OutlinePassData {
            RG::ResourceHandle output;
            RG::ResourceHandle entityIDInput;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<OutlinePassData>("OutlinePass",
            [&](OutlinePassData& data, RG::RenderPassBuilder& builder)
            {
                // Write to LDR output (alpha-blend outline on top)
                data.output = builder.Write(ldrOutput,
                    VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);

                // Read entity ID buffer as sampled texture
                data.entityIDInput = builder.Read(entityIDHandle);

                outputHandle = data.output;
            },
            [this](OutlinePassData& data, RG::RenderPassContext& ctx)
            {
                // Look up the selected entity's index in the entity lookup table
                // (must happen at execute time — m_EntityLookup is populated by the geometry pass)
                u32 selectedIdx = 0;
                if (m_SelectedEntity != entt::null)
                {
                    for (u32 i = 1; i < (u32)m_EntityLookup.size(); ++i)
                    {
                        if (m_EntityLookup[i] == m_SelectedEntity)
                        {
                            selectedIdx = i;
                            break;
                        }
                    }
                }

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

                // Push constants: selectedEntityID, outlineWidth, texelSize, outlineColor
                struct OutlinePushConstants {
                    u32   selectedEntityID;
                    float outlineWidth;
                    float texelSizeX;
                    float texelSizeY;
                    float outlineColorR;
                    float outlineColorG;
                    float outlineColorB;
                    float outlineColorA;
                } pc;

                pc.selectedEntityID = selectedIdx;
                pc.outlineWidth     = 2.0f;
                pc.texelSizeX       = 1.0f / (float)w;
                pc.texelSizeY       = 1.0f / (float)h;
                pc.outlineColorR    = 1.0f; // Orange outline
                pc.outlineColorG    = 0.6f;
                pc.outlineColorB    = 0.0f;
                pc.outlineColorA    = 1.0f;

                vkCmdPushConstants(cmd, m_OutlinePipeline->GetLayout(),
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

                vkCmdDraw(cmd, 3, 1, 0, 0);
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
            [&](ImGuiPassData& data, RG::RenderPassContext& ctx)
            {
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), ctx.commandBuffer);
            }
        );
    }

    // =========================================================================
    // Frame Debugger helpers
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
            UpdatePostProcessDescriptors();

            // Update outline descriptor with new entity ID buffer
            if (m_OutlineDescSet && m_OutlineSampler)
            {
                auto vkEntityID = std::static_pointer_cast<VKTexture>(m_EntityIDBuffer);
                VkDescriptorImageInfo entityIDImgInfo{};
                entityIDImgInfo.sampler     = m_OutlineSampler;
                entityIDImgInfo.imageView   = vkEntityID->GetImageView();
                entityIDImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet outlineWrite{};
                outlineWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                outlineWrite.dstSet = m_OutlineDescSet;
                outlineWrite.dstBinding = 0;
                outlineWrite.descriptorCount = 1;
                outlineWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                outlineWrite.pImageInfo = &entityIDImgInfo;
                vkUpdateDescriptorSets(VulkanContext::Get().GetDevice(), 1, &outlineWrite, 0, nullptr);
            }

            RegisterNamedTextures();
        }
    }
}
