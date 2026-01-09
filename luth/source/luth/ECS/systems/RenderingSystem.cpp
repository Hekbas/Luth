#include "luthpch.h"
#include "luth/ECS/systems/RenderingSystem.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Profiler.h"
#include "luth/ECS/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VKRendererAPI.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanTexture.h"
#include "luth/renderer/backend/vulkan/VulkanBuffer.h"
#include "luth/renderer/Model.h"
#include "luth/resources/libraries/ModelLibrary.h"
#include "luth/resources/FileSystem.h"
#include "luth/renderer/ShaderCompiler.h"
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

namespace Luth
{
    RenderingSystem::RenderingSystem(u32 viewportWidth, u32 viewportHeight)
    {
        // Initialize Frame Allocator (1MB should be enough for command lists for now)
        m_FrameAllocator = std::make_unique<LinearAllocator>(1 * Memory::MB);

        if (Renderer::GetAPI() == RendererAPI::API::Vulkan)
        {
            // Create Scene Color Texture
            m_SceneColor = Texture::Create(viewportWidth, viewportHeight, TextureFormat::RGBA8);

            InitGlobalUniforms();

            // Compile Shaders
            fs::path vertSrc = FileSystem::AssetsPath() / "shaders/triangle.vert";
            fs::path fragSrc = FileSystem::AssetsPath() / "shaders/triangle.frag";
            
            std::vector<u32> vertSpv = ShaderCompiler::Compile(vertSrc);
            std::vector<u32> fragSpv = ShaderCompiler::Compile(fragSrc);

            PipelineConfig config;
            config.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM }; // SceneColor format
            config.depthFormat = VK_FORMAT_D32_SFLOAT;
            config.depthTest = true;
            config.depthWrite = true;
            config.cullMode = VK_CULL_MODE_BACK_BIT;

            // Layouts: Set 0 = Bindless, Set 1 = Global Uniforms
            std::vector<VkDescriptorSetLayout> layouts = {
                VulkanContext::Get().GetBindlessSet().GetLayout(),
                m_GlobalSetLayout
            };

            m_TrianglePipeline = std::make_unique<VKPipeline>(config, vertSpv, fragSpv, layouts);
        }
    }

    RenderingSystem::~RenderingSystem()
    {
        // Allocator cleans itself up
        if (m_GlobalSetLayout) vkDestroyDescriptorSetLayout(VulkanContext::Get().GetDevice(), m_GlobalSetLayout, nullptr);
    }

    void RenderingSystem::InitGlobalUniforms()
    {
        VkDevice device = VulkanContext::Get().GetDevice();

        // 1. Create Buffer
        m_GlobalUniformBuffer = std::make_shared<VKUniformBuffer>(sizeof(GlobalUniforms));

        // 2. Create Layout
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

        // 3. Allocate Set
        VulkanContext::Get().GetDescriptorAllocator().Allocate(m_GlobalSetLayout, m_GlobalDescriptorSet);

        // 4. Write Set
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
        ubo.projection[1][1] *= -1; // Vulkan flip Y
        ubo.viewProjection = ubo.projection * ubo.view;
        ubo.cameraPos = camera.GetPosition();
        ubo.time = Time::GetTime();

        m_GlobalUniformBuffer->SetData(&ubo, sizeof(GlobalUniforms));
    }

    void RenderingSystem::Update(entt::registry& registry)
    {
        LH_PROFILE_FUNCTION();

        // Reset Allocator at start of frame
        m_FrameAllocator->Reset();

        // -----------------------------------------------------------------
        // Render Graph Test (Proof of Concept)
        // -----------------------------------------------------------------
        if (Renderer::GetAPI() == RendererAPI::API::Vulkan)
        {
            if (!Renderer::BeginFrame())
                return;

            UpdateGlobalUniforms();

            RG::RenderGraph rg(*m_FrameAllocator);

            struct GeometryPassData {
                RG::ResourceHandle outputTex;
                RG::ResourceHandle depthTex;
            };

            static RG::ResourceHandle s_SceneColorHandle;

            rg.AddPass<GeometryPassData>("GeometryPass",
                [&](GeometryPassData& data, RG::RenderPassBuilder& builder)
                {
                    RG::TextureDesc desc;
                    desc.name = "SceneColor";
                    desc.width = m_SceneColor->GetWidth();
                    desc.height = m_SceneColor->GetHeight();
                    desc.format = RG::TextureFormat::RGBA8_Unorm;
                    
                    data.outputTex = rg.ImportResource(desc, 
                        (void*)static_cast<VKTexture*>(m_SceneColor.get())->GetImage(), 
                        (void*)static_cast<VKTexture*>(m_SceneColor.get())->GetImageView(), 
                        RG::ResourceState::ShaderResource);

                    RG::TextureDesc depthDesc;
                    depthDesc.name = "SceneDepth";
                    depthDesc.width = m_SceneColor->GetWidth();
                    depthDesc.height = m_SceneColor->GetHeight();
                    depthDesc.format = RG::TextureFormat::D32_Float;

                    data.depthTex = builder.CreateTexture(depthDesc);
                    data.depthTex = builder.Write(data.depthTex);

                    data.outputTex = builder.Write(data.outputTex);
                    s_SceneColorHandle = data.outputTex;
                },
                [&](GeometryPassData& data, RG::RenderPassContext& ctx)
                {
                    VkCommandBuffer cmd = ctx.commandBuffer;
                    RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.outputTex);
                    RG::RenderGraph::ResourceNode* depthRes = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.depthTex);

                    if (res)
                    {
                        // Begin Rendering (Dynamic Rendering)
                        VkRenderingAttachmentInfo colorAttachment{};
                        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                        colorAttachment.imageView = res->view; // Now accessible
                        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                        colorAttachment.clearValue = { {{0.1f, 0.1f, 0.1f, 1.0f}} }; // Dark Grey Background

                        VkRenderingAttachmentInfo depthAttachment{};
                        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                        depthAttachment.imageView = depthRes->view;
                        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                        depthAttachment.clearValue.depthStencil = { 1.0f, 0 };

                        VkRenderingInfo renderingInfo{};
                        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                        renderingInfo.renderArea = { {0, 0}, {res->desc.width, res->desc.height} };
                        renderingInfo.layerCount = 1;
                        renderingInfo.colorAttachmentCount = 1;
                        renderingInfo.pColorAttachments = &colorAttachment;
                        renderingInfo.pDepthAttachment = &depthAttachment;

                        vkCmdBeginRendering(cmd, &renderingInfo);

                        // Bind Pipeline
                        m_TrianglePipeline->Bind(cmd);

                        // Bind Bindless Descriptor Set (Set 0)
                        VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TrianglePipeline->GetLayout(), 0, 1, &bindlessSet, 0, nullptr);

                        // Bind Global Uniforms (Set 1)
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TrianglePipeline->GetLayout(), 1, 1, &m_GlobalDescriptorSet, 0, nullptr);

                        // Set Dynamic States
                        VkViewport viewport{};
                        viewport.x = 0.0f;
                        viewport.y = 0.0f;
                        viewport.width = (float)res->desc.width;
                        viewport.height = (float)res->desc.height;
                        viewport.minDepth = 0.0f;
                        viewport.maxDepth = 1.0f;
                        vkCmdSetViewport(cmd, 0, 1, &viewport);

                        VkRect2D scissor{};
                        scissor.offset = { 0, 0 };
                        scissor.extent = { res->desc.width, res->desc.height };
                        vkCmdSetScissor(cmd, 0, 1, &scissor);

                        // Draw Entities
                        auto view = registry.view<Transform, MeshRenderer>();
                        for (auto [entity, transform, meshRenderer] : view.each())
                        {
                            auto model = ModelLibrary::Get(meshRenderer.ModelUUID);
                            if (!model) continue;

                            auto mesh = model->GetMesh(meshRenderer.MeshIndex);
                            if (!mesh) continue;

                            // Get Vulkan Buffers
                            auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                            auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());

                            if (!vb || !ib) continue;
                            
                            // Push Constants (Model Matrix)
                            Mat4 modelMatrix = transform.GetTransform();
                            vkCmdPushConstants(cmd, m_TrianglePipeline->GetLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Mat4), &modelMatrix);

                            // Bind Vertex Buffer
                            VkBuffer vertexBuffers[] = { vb->GetVulkanBuffer() };
                            VkDeviceSize offsets[] = { 0 };
                            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

                            // Bind Index Buffer
                            vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                            // Draw
                            vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);
                        }

                        vkCmdEndRendering(cmd);
                    }
                }
            );

            // 2. ImGui Pass (Draws UI on top of Swapchain)
            struct ImGuiPassData {
                RG::ResourceHandle backbuffer;
            };

            rg.AddPass<ImGuiPassData>("ImGuiPass",
                [&](ImGuiPassData& data, RG::RenderPassBuilder& builder)
                {
                    auto* vkRenderer = static_cast<VKRendererAPI*>(Renderer::GetRendererAPI());
                    VkImage swapchainImage = vkRenderer->GetSwapchain().GetImage(vkRenderer->GetSwapchain().GetCurrentFrameIndex());
                    VkImageView swapchainView = vkRenderer->GetSwapchain().GetImageView(vkRenderer->GetSwapchain().GetCurrentFrameIndex());

                    RG::TextureDesc desc;
                    desc.name = "Backbuffer";
                    desc.width = vkRenderer->GetSwapchain().GetExtent().width;
                    desc.height = vkRenderer->GetSwapchain().GetExtent().height;
                    desc.format = RG::TextureFormat::RGBA8_Unorm;
                    
                    data.backbuffer = rg.ImportResource(desc, 
                        (void*)swapchainImage, 
                        (void*)swapchainView, 
                        RG::ResourceState::Undefined);
                    data.backbuffer = builder.Write(data.backbuffer);

                    // Declare dependency on SceneColor so RenderGraph generates the transition to SHADER_READ_ONLY
                    if (s_SceneColorHandle.IsValid())
                    {
                        builder.Read(s_SceneColorHandle);
                    }
                },
                [&](ImGuiPassData& data, RG::RenderPassContext& ctx)
                {
                    VkCommandBuffer cmd = ctx.commandBuffer;
                    RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.backbuffer);

                    VkRenderingAttachmentInfo colorAttachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
                    colorAttachment.imageView = res->view;
                    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    colorAttachment.clearValue = { 0.0f, 0.0f, 0.0f, 1.0f };

                    VkRenderingInfo renderInfo = { VK_STRUCTURE_TYPE_RENDERING_INFO };
                    renderInfo.renderArea = { {0, 0}, {res->desc.width, res->desc.height} };
                    renderInfo.layerCount = 1;
                    renderInfo.colorAttachmentCount = 1;
                    renderInfo.pColorAttachments = &colorAttachment;

                    vkCmdBeginRendering(cmd, &renderInfo);
                    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
                    vkCmdEndRendering(cmd);
                }
            );

            rg.Compile();
            Renderer::ExecuteGraph(rg);
            Renderer::EndFrame();
            
            return;
        }
    }

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        if (m_SceneColor)
        {
            // Recreate texture
            m_SceneColor = Texture::Create(width, height, TextureFormat::RGBA8);
        }
    }
}
