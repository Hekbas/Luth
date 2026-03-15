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
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

namespace Luth
{
    using namespace Component;

    RenderingSystem::RenderingSystem(u32 viewportWidth, u32 viewportHeight)
    {
        m_FrameAllocator = std::make_unique<Memory::LinearAllocator>(1 * Memory::MB);

        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan)
        {
            m_SceneColor = Texture::Create(viewportWidth, viewportHeight, TextureFormat::RGBA8);

            InitGlobalUniforms();

            // Compile PBR shaders
            fs::path vertSrc = FileSystem::AssetsPath() / "shaders/pbr.vert";
            fs::path fragSrc = FileSystem::AssetsPath() / "shaders/pbr.frag";

            m_PBRVertSpv = ShaderCompiler::Compile(vertSrc);
            m_PBRFragSpv = ShaderCompiler::Compile(fragSrc);

            if (m_PBRVertSpv.empty() || m_PBRFragSpv.empty())
            {
                LH_CORE_ERROR("Failed to compile PBR shaders!");
                return;
            }

            CreatePipelines();
        }
    }

    RenderingSystem::~RenderingSystem()
    {
        if (m_GlobalSetLayout)
            vkDestroyDescriptorSetLayout(VulkanContext::Get().GetDevice(), m_GlobalSetLayout, nullptr);
    }

    void RenderingSystem::CreatePipelines()
    {
        // Vertex layout (matches Model::ProcessMeshData)
        BufferLayout vertexLayout = {
            { ShaderDataType::Float3, "a_Position"  },
            { ShaderDataType::Float3, "a_Normal"    },
            { ShaderDataType::Float2, "a_TexCoord0" },
            { ShaderDataType::Float2, "a_TexCoord1" },
            { ShaderDataType::Float3, "a_Tangent"   }
        };

        auto bindingDescs = vertexLayout.GetBindingDescriptions();
        auto attribDescs  = vertexLayout.GetAttributeDescriptions();

        // Push constants
        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(ObjectPushConstants);

        // Descriptor set layouts: Set 0 = Global UBO, Set 1 = Bindless, Set 2 = Material SSBO
        std::vector<VkDescriptorSetLayout> layouts = {
            m_GlobalSetLayout,
            VulkanContext::Get().GetBindlessSet().GetLayout(),
            MaterialSystem::GetDescriptorSetLayout()
        };

        // Shared base config
        PipelineConfig baseConfig;
        baseConfig.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM };
        baseConfig.depthFormat = VK_FORMAT_D32_SFLOAT;
        baseConfig.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        baseConfig.bindingDescriptions = bindingDescs;
        baseConfig.attributeDescriptions = attribDescs;
        baseConfig.pushConstantRanges = { pushConstantRange };

        // Opaque pipeline
        {
            PipelineConfig config = baseConfig;
            config.depthTest = true;
            config.depthWrite = true;
            config.blendEnabled = false;
            config.cullMode = VK_CULL_MODE_BACK_BIT;
            m_Pipelines[Material::RenderMode::Opaque] =
                std::make_unique<VKPipeline>(config, m_PBRVertSpv, m_PBRFragSpv, layouts);
        }

        // Cutout pipeline (two-sided for foliage/hair)
        {
            PipelineConfig config = baseConfig;
            config.depthTest = true;
            config.depthWrite = true;
            config.blendEnabled = false;
            config.cullMode = VK_CULL_MODE_NONE;
            m_Pipelines[Material::RenderMode::Cutout] =
                std::make_unique<VKPipeline>(config, m_PBRVertSpv, m_PBRFragSpv, layouts);
        }

        // Transparent pipeline
        {
            PipelineConfig config = baseConfig;
            config.depthTest = true;
            config.depthWrite = false;
            config.blendEnabled = true;
            config.cullMode = VK_CULL_MODE_NONE;
            m_Pipelines[Material::RenderMode::Transparent] =
                std::make_unique<VKPipeline>(config, m_PBRVertSpv, m_PBRFragSpv, layouts);
        }
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

        m_GlobalUniformBuffer->SetData(&ubo, sizeof(GlobalUniforms));
    }

    void RenderingSystem::Update(Scene* scene)
    {
        LH_PROFILE_FUNCTION();
        auto& registry = scene->Registry();

        m_FrameAllocator->Reset();

        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan)
        {
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

            // Upload dirty materials to GPU (buffer is persistently mapped)
            MaterialSystem::Update(VK_NULL_HANDLE);

            RG::RenderGraph rg(*m_FrameAllocator);

            RG::ResourceHandle sceneColor = AddGeometryPass(rg, registry);
            AddImGuiPass(rg, sceneColor);

            rg.Compile();
            Renderer::ExecuteGraph(rg, Renderer::GetFrameData()->GetFrameIndex());

            return;
        }
    }

    RG::ResourceHandle RenderingSystem::AddGeometryPass(RG::RenderGraph& rg, entt::registry& registry)
    {
        struct GeometryPassData {
            RG::ResourceHandle outputTex;
            RG::ResourceHandle depthTex;
        };

        RG::ResourceHandle outputHandle;

        rg.AddPass<GeometryPassData>("GeometryPass",
            [&](GeometryPassData& data, RG::RenderPassBuilder& builder)
            {
                RG::TextureDesc desc;
                desc.name = "SceneColor";
                desc.width = m_SceneColor->GetWidth();
                desc.height = m_SceneColor->GetHeight();
                desc.format = RG::TextureFormat::RGBA8_Unorm;

                auto vkTex = std::static_pointer_cast<VKTexture>(m_SceneColor);
                data.outputTex = rg.ImportResource(desc,
                    (void*)vkTex->GetImage(),
                    (void*)vkTex->GetImageView(),
                    RG::ResourceState::ShaderResource);

                RG::TextureDesc depthDesc;
                depthDesc.name = "SceneDepth";
                depthDesc.width = m_SceneColor->GetWidth();
                depthDesc.height = m_SceneColor->GetHeight();
                depthDesc.format = RG::TextureFormat::D32_Float;

                data.depthTex = builder.CreateTexture(depthDesc);

                VkClearValue depthClear{};
                depthClear.depthStencil = { 1.0f, 0 };
                data.depthTex = builder.WriteDepth(data.depthTex,
                    VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, depthClear);
                data.outputTex = builder.Write(data.outputTex);

                outputHandle = data.outputTex;
            },
            [this, &registry](GeometryPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;

                // Get a pipeline layout (all variants share the same layout)
                auto& opaquePipeline = m_Pipelines[Material::RenderMode::Opaque];
                if (!opaquePipeline) return;
                VkPipelineLayout pipelineLayout = opaquePipeline->GetLayout();

                // Bind descriptor sets: Set 0 = Global UBO, Set 1 = Bindless, Set 2 = Material SSBO
                VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                VkDescriptorSet materialSet = MaterialSystem::GetDescriptorSet();
                VkDescriptorSet sets[] = { m_GlobalDescriptorSet, bindlessSet, materialSet };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout, 0, 3, sets, 0, nullptr);

                // Set viewport/scissor
                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.outputTex);

                VkViewport viewport{};
                viewport.width = (float)res->desc.width;
                viewport.height = (float)res->desc.height;
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
                    dc.modelMatrix = worldTransform.Matrix;
                    dc.materialSlot = 0;
                    dc.model = model;
                    dc.meshIndex = meshRenderer.MeshIndex;

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

                // Draw helper lambda
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
                        pc.modelMatrix = dc.modelMatrix;
                        pc.materialIndex = dc.materialSlot;

                        vkCmdPushConstants(cmd, pipelineLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(ObjectPushConstants), &pc);

                        VkBuffer vertexBuffers[] = { vb->GetVulkanBuffer() };
                        VkDeviceSize offsets[] = { 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
                        vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);
                        vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);
                    }
                };

                // Draw order: Opaque → Cutout → Transparent
                DrawBatch(opaqueDraws, Material::RenderMode::Opaque);
                DrawBatch(cutoutDraws, Material::RenderMode::Cutout);
                DrawBatch(transparentDraws, Material::RenderMode::Transparent);
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
                VkImage swapchainImage = vkRenderer->GetSwapchain().GetImage(vkRenderer->GetSwapchain().GetCurrentFrameIndex());
                VkImageView swapchainView = vkRenderer->GetSwapchain().GetImageView(vkRenderer->GetSwapchain().GetCurrentFrameIndex());

                RG::TextureDesc desc;
                desc.name = "Backbuffer";
                desc.width = vkRenderer->GetSwapchain().GetExtent().width;
                desc.height = vkRenderer->GetSwapchain().GetExtent().height;
                desc.format = RG::TextureFormat::BGRA8_Unorm;

                data.backbuffer = rg.ImportResource(desc,
                    (void*)swapchainImage,
                    (void*)swapchainView,
                    RG::ResourceState::Undefined);

                data.backbuffer = builder.Write(data.backbuffer);

                if (sceneColor.IsValid())
                {
                    data.sceneTexture = builder.Read(sceneColor);
                }
            },
            [&](ImGuiPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
            }
        );
    }

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        if (m_SceneColor && width > 0 && height > 0)
        {
            m_SceneColor = Texture::Create(width, height, TextureFormat::RGBA8);
        }
    }
}
