#include "luthpch.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/Profiler.h"
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
#include "luth/renderer/ShaderCompiler.h"
#include "luth/renderer/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/VulkanShader.h"
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

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
            m_SceneColor = Texture::Create(viewportWidth, viewportHeight, TextureFormat::RGBA8);

            InitGlobalUniforms();
            InitShadowResources();

            // Create and register shaders via Shader asset (compiles + reflects)
            auto pbrShader = Shader::Create(FileSystem::AssetsPath() / "shaders/pbr.vert");
            ShaderLibrary::Register("pbr", pbrShader);

            auto shadowShader = Shader::Create(FileSystem::AssetsPath() / "shaders/shadowDepth.vert");
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

                m_Pipelines.clear();
                m_ShadowPipeline.reset();
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
        }
    }

    RenderingSystem::~RenderingSystem()
    {
        m_ShaderWatcher.Stop();
        ShaderLibrary::SetReloadCallback(nullptr);

        VkDevice device = VulkanContext::Get().GetDevice();

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

        VkDescriptorSetLayoutBinding uboBinding{};
        uboBinding.binding = 0;
        uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboBinding.descriptorCount = 1;
        uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &uboBinding;

        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_GlobalSetLayout);

        VulkanContext::Get().GetDescriptorAllocator().Allocate(m_GlobalSetLayout, m_GlobalDescriptorSet);

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
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

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

        // ---- PBR geometry pipelines ----
        PipelineConfig baseConfig;
        baseConfig.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
        baseConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
        baseConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        baseConfig.bindingDescriptions = bindingDescs;
        baseConfig.attributeDescriptions = attribDescs;
        baseConfig.pushConstantRanges = { pushConstantRange };

        {
            PipelineConfig config = baseConfig;
            config.depthTest = true; config.depthWrite = true;
            config.blendEnabled = false; config.cullMode = VK_CULL_MODE_BACK_BIT;
            m_Pipelines[Material::RenderMode::Opaque] =
                std::make_unique<VKPipeline>(config, m_PBRVertSpv, m_PBRFragSpv, layouts);
        }
        {
            PipelineConfig config = baseConfig;
            config.depthTest = true; config.depthWrite = true;
            config.blendEnabled = false; config.cullMode = VK_CULL_MODE_NONE;
            m_Pipelines[Material::RenderMode::Cutout] =
                std::make_unique<VKPipeline>(config, m_PBRVertSpv, m_PBRFragSpv, layouts);
        }
        {
            PipelineConfig config = baseConfig;
            config.depthTest = true; config.depthWrite = false;
            config.blendEnabled = true; config.cullMode = VK_CULL_MODE_NONE;
            m_Pipelines[Material::RenderMode::Transparent] =
                std::make_unique<VKPipeline>(config, m_PBRVertSpv, m_PBRFragSpv, layouts);
        }

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
    }

    u32 RenderingSystem::EnsureMaterialRegistered(Material* material)
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
                m_CachedCastShadows = dl.CastShadows;
                m_CachedShadowBias  = dl.ShadowBias;
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
        constexpr float orthoSize = 50.0f;
        auto scenePanel = Editor::GetPanel<ScenePanel>();
        glm::vec3 camPos = scenePanel ? scenePanel->GetEditorCamera().GetPosition() : glm::vec3(0.0f);

        glm::vec3 lightDir = lights.dirLight.direction;
        glm::vec3 lightPos = camPos - lightDir * 50.0f;
        glm::vec3 up = (glm::abs(glm::dot(lightDir, glm::vec3(0, 1, 0))) > 0.99f)
                       ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);

        glm::mat4 lightView = glm::lookAt(lightPos, lightPos + lightDir, up);
        glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, -50.0f, 100.0f);
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

            // Register materials for all visible entities
            auto matView = registry.view<WorldTransform, MeshRenderer>();
            for (auto [entity, wt, mr] : matView.each())
            {
                if (!mr.MaterialUUID.IsValid()) continue;
                auto material = AssetManager::GetAsset<Material>(mr.MaterialUUID);
                if (material)
                    EnsureMaterialRegistered(material.get());
            }

            // Upload dirty materials
            MaterialSystem::Update(VK_NULL_HANDLE);

            RG::RenderGraph rg(*m_FrameAllocator);

            RG::ResourceHandle shadowMap  = AddShadowPass(rg, registry);
            RG::ResourceHandle sceneColor = AddGeometryPass(rg, registry, shadowMap);
            AddImGuiPass(rg, sceneColor);

            rg.Compile();
            Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex());

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

    RG::ResourceHandle RenderingSystem::AddGeometryPass(
        RG::RenderGraph& rg, entt::registry& registry, RG::ResourceHandle shadowMapHandle)
    {
        struct GeometryPassData {
            RG::ResourceHandle outputTex;
            RG::ResourceHandle depthTex;
            RG::ResourceHandle shadowTex;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<GeometryPassData>("GeometryPass",
            [&](GeometryPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name   = "SceneColor";
                desc.width  = m_SceneColor->GetWidth();
                desc.height = m_SceneColor->GetHeight();
                desc.format = RG::TextureFormat::RGBA8_Unorm;

                auto vkTex = std::static_pointer_cast<VKTexture>(m_SceneColor);
                data.outputTex = rg.ImportResource(desc,
                    (void*)vkTex->GetImage(),
                    (void*)vkTex->GetImageView(),
                    RG::ResourceState::ShaderResource);

                RG::TextureDesc depthDesc;
                depthDesc.name   = "SceneDepth";
                depthDesc.width  = m_SceneColor->GetWidth();
                depthDesc.height = m_SceneColor->GetHeight();
                depthDesc.format = RG::TextureFormat::D32_Float;

                data.depthTex = builder.CreateTexture(depthDesc);

                VkClearValue depthClear{};
                depthClear.depthStencil = { 1.0f, 0 };
                data.depthTex  = builder.WriteDepth(data.depthTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, depthClear);
                data.outputTex = builder.Write(data.outputTex);

                // Declare dependency on the shadow map (triggers depth→shader_read barrier)
                if (shadowMapHandle.IsValid())
                    data.shadowTex = builder.Read(shadowMapHandle);

                outputHandle = data.outputTex;
            },
            [this, &registry](GeometryPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                auto& opaquePipeline = m_Pipelines[Material::RenderMode::Opaque];
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

                // Collect draw commands per render mode
                struct DrawCommand {
                    glm::mat4 modelMatrix;
                    u32 materialSlot;
                    std::shared_ptr<Model> model;
                    u32 meshIndex;
                };

                std::vector<DrawCommand> opaqueDraws;
                std::vector<DrawCommand> cutoutDraws;
                std::vector<DrawCommand> transparentDraws;

                auto view = registry.view<WorldTransform, MeshRenderer>();
                for (auto [entity, worldTransform, meshRenderer] : view.each())
                {
                    auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID);
                    if (!model) continue;
                    if (!model->GetMesh(meshRenderer.MeshIndex)) continue;

                    DrawCommand dc;
                    dc.modelMatrix  = worldTransform.Matrix;
                    dc.materialSlot = 0;
                    dc.model        = model;
                    dc.meshIndex    = meshRenderer.MeshIndex;

                    Material::RenderMode mode = Material::RenderMode::Opaque;

                    if (meshRenderer.MaterialUUID.IsValid())
                    {
                        auto material = AssetManager::GetAsset<Material>(meshRenderer.MaterialUUID);
                        if (material)
                        {
                            auto slotIt = m_MaterialSlotMap.find(material->Handle);
                            if (slotIt != m_MaterialSlotMap.end())
                                dc.materialSlot = slotIt->second;
                            mode = material->GetRenderMode();
                        }
                    }

                    switch (mode)
                    {
                        case Material::RenderMode::Cutout:      cutoutDraws.push_back(dc);      break;
                        case Material::RenderMode::Transparent:
                        case Material::RenderMode::Fade:        transparentDraws.push_back(dc); break;
                        default:                                opaqueDraws.push_back(dc);       break;
                    }
                }

                auto DrawBatch = [&](const std::vector<DrawCommand>& draws, Material::RenderMode mode)
                {
                    if (draws.empty()) return;

                    auto pipeIt = m_Pipelines.find(mode);
                    if (pipeIt == m_Pipelines.end() || !pipeIt->second) return;

                    pipeIt->second->Bind(cmd);

                    for (const auto& dc : draws)
                    {
                        auto mesh = dc.model->GetMesh(dc.meshIndex);
                        auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                        auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());
                        if (!vb || !ib) continue;

                        ObjectPushConstants pc{};
                        pc.modelMatrix  = dc.modelMatrix;
                        pc.materialIndex = dc.materialSlot;

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

                DrawBatch(opaqueDraws,       Material::RenderMode::Opaque);
                DrawBatch(cutoutDraws,        Material::RenderMode::Cutout);
                DrawBatch(transparentDraws,   Material::RenderMode::Transparent);
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
    // Resize
    // =========================================================================

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        if (m_SceneColor && width > 0 && height > 0)
            m_SceneColor = Texture::Create(width, height, TextureFormat::RGBA8);
    }
}
