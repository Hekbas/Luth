#include "luthpch.h"
#include "luth/ECS/systems/RenderingSystem.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Profiler.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/backend/vulkan/VKRendererAPI.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/resources/FileSystem.h"

namespace Luth
{
    RenderingSystem::RenderingSystem(u32 viewportWidth, u32 viewportHeight)
    {
        // Initialize Frame Allocator (1MB should be enough for command lists for now)
        m_FrameAllocator = std::make_unique<LinearAllocator>(1 * Memory::MB);

        if (Renderer::GetAPI() == RendererAPI::API::Vulkan)
        {
            // Load compiled SPIR-V shaders
            std::string vertPath = (FileSystem::AssetsPath() / "shaders/spv/triangle.vert.spv").string();
            std::string fragPath = (FileSystem::AssetsPath() / "shaders/spv/triangle.frag.spv").string();

            // Check if files exist
            if (!fs::exists(vertPath) || !fs::exists(fragPath)) {
                LH_CORE_ERROR("Shader files not found! Run compile_shaders.bat");
                return;
            }

            PipelineConfig config;
            config.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM }; // SceneColor format
            config.depthFormat = VK_FORMAT_UNDEFINED; // No depth for triangle test
            config.depthTest = false;
            config.depthWrite = false;
            config.cullMode = VK_CULL_MODE_NONE; // Disable culling to be safe

            // Use global bindless layout
            VkDescriptorSetLayout globalLayout = VulkanContext::Get().GetBindlessSet().GetLayout();
            m_TrianglePipeline = std::make_unique<VKPipeline>(config, vertPath, fragPath, globalLayout);
        }
    }

    RenderingSystem::~RenderingSystem()
    {
        // Allocator cleans itself up
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
            Renderer::BeginFrame();

            RG::RenderGraph rg(*m_FrameAllocator);

            struct GeometryPassData {
                RG::ResourceHandle outputTex;
            };

            // 1. Geometry Pass (Draws Triangle)
            static RG::ResourceHandle s_SceneColorHandle;

            rg.AddPass<GeometryPassData>("GeometryPass",
                [&](GeometryPassData& data, RG::RenderPassBuilder& builder)
                {
                    RG::TextureDesc desc;
                    desc.name = "SceneColor";
                    desc.width = 1280;
                    desc.height = 720;
                    desc.format = RG::TextureFormat::RGBA8_Unorm;
                    
                    data.outputTex = builder.CreateTexture(desc);
                    data.outputTex = builder.Write(data.outputTex);
                    s_SceneColorHandle = data.outputTex;
                },
                [&](GeometryPassData& data, RG::RenderPassContext& ctx)
                {
                    VkCommandBuffer cmd = ctx.commandBuffer;
                    RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.outputTex);

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

                        VkRenderingInfo renderingInfo{};
                        renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                        renderingInfo.renderArea = { {0, 0}, {res->desc.width, res->desc.height} };
                        renderingInfo.layerCount = 1;
                        renderingInfo.colorAttachmentCount = 1;
                        renderingInfo.pColorAttachments = &colorAttachment;

                        vkCmdBeginRendering(cmd, &renderingInfo);

                        // Bind Pipeline
                        m_TrianglePipeline->Bind(cmd);

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

                        // Draw Triangle (3 vertices)
                        vkCmdDraw(cmd, 3, 1, 0, 0);

                        vkCmdEndRendering(cmd);
                    }
                }
            );

            // 2. Present Pass (Clears Backbuffer using Render Pass)
            struct PresentPassData {
                RG::ResourceHandle backbuffer;
            };

            rg.AddPass<PresentPassData>("PresentPass",
                [&](PresentPassData& data, RG::RenderPassBuilder& builder)
                {
                    RG::TextureDesc desc;
                    desc.name = "Backbuffer";
                    desc.width = 1280;
                    desc.height = 720;
                    desc.format = RG::TextureFormat::RGBA8_Unorm;
                    
                    // Import Swapchain Image
                    // Note: In a real engine, we would get the current swapchain image from Renderer
                    // For now, we assume VKRendererAPI exposes it or we just clear it here.
                    // Since we don't have easy access to the swapchain image handle here without casting,
                    // we will skip the actual import logic for this specific test and just rely on the fact
                    // that EndFrame() presents.
                    
                    // However, to test the graph, we can just use the output of GeometryPass
                    // In a real scenario, we would Blit s_SceneColorHandle to Swapchain Image.
                },
                [&](PresentPassData& data, RG::RenderPassContext& ctx)
                {
                    // Placeholder for Swapchain Blit
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
        // Resize logic handled by RenderGraph recreation usually
    }
}
