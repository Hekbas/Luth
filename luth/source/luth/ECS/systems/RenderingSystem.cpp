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
#include "luth/renderer/Material.h"
#include "luth/renderer/Model.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/Buffer.h"
#include "luth/resources/FileSystem.h"
#include "luth/renderer/ShaderCompiler.h"
#include <fstream>
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
            
            // Ensure default shaders exist
            if (!fs::exists(vertSrc.parent_path())) fs::create_directories(vertSrc.parent_path());
            
            if (!fs::exists(vertSrc)) {
                std::ofstream out(vertSrc);
                out << R"(#version 450
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord0;
layout(location = 3) in vec2 a_TexCoord1;
layout(location = 4) in vec3 a_Tangent;

layout(location = 0) out vec3 v_Normal;
layout(location = 1) out vec2 v_TexCoord;

layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 modelMatrix;
    uint albedoMapIndex;
} pc;

void main() {
    v_Normal = mat3(transpose(inverse(pc.modelMatrix))) * a_Normal;
    v_TexCoord = a_TexCoord0;
    gl_Position = ubo.viewProjection * pc.modelMatrix * vec4(a_Position, 1.0);
})";
            }

            if (!fs::exists(fragSrc)) {
                std::ofstream out(fragSrc);
                out << R"(#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 v_Normal;
layout(location = 1) in vec2 v_TexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
} ubo;

layout(set = 1, binding = 0) uniform sampler2D globalTextures[];

layout(push_constant) uniform PushConstants {
    mat4 modelMatrix;
    uint albedoMapIndex;
} pc;

void main() {
    vec3 lightDir = normalize(vec3(1.0, 1.0, 1.0));
    float diff = max(dot(normalize(v_Normal), lightDir), 0.1);
    
    // Bindless lookup
    vec4 albedo = texture(globalTextures[nonuniformEXT(pc.albedoMapIndex)], v_TexCoord);
    
    outColor = vec4(albedo.rgb * diff, albedo.a);
})";
            }

            std::vector<u32> vertSpv = ShaderCompiler::Compile(vertSrc);
            std::vector<u32> fragSpv = ShaderCompiler::Compile(fragSrc);

            PipelineConfig config;
            config.colorFormats = { VK_FORMAT_R8G8B8A8_UNORM }; // SceneColor format
            config.depthFormat = VK_FORMAT_D32_SFLOAT;
            config.depthTest = true;
            config.depthWrite = true;
            config.cullMode = VK_CULL_MODE_BACK_BIT;

            // Define Vertex Layout (Matches Model::ProcessMeshData)
            BufferLayout vertexLayout = {
                { ShaderDataType::Float3, "a_Position"  },
                { ShaderDataType::Float3, "a_Normal"    },
                { ShaderDataType::Float2, "a_TexCoord0" },
                { ShaderDataType::Float2, "a_TexCoord1" },
                { ShaderDataType::Float3, "a_Tangent"   } 
            };
            config.bindingDescriptions = vertexLayout.GetBindingDescriptions();
            config.attributeDescriptions = vertexLayout.GetAttributeDescriptions();

            // Push Constants
            VkPushConstantRange pushConstantRange{};
            pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            pushConstantRange.offset = 0;
            pushConstantRange.size = sizeof(ObjectPushConstants);
            config.pushConstantRanges = { pushConstantRange };

            // Layouts: Set 0 = Global Uniforms, Set 1 = Bindless
            std::vector<VkDescriptorSetLayout> layouts = {
                m_GlobalSetLayout,
                VulkanContext::Get().GetBindlessSet().GetLayout()
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

            RG::ResourceHandle sceneColor = AddGeometryPass(rg, registry);
            AddImGuiPass(rg, sceneColor);

            rg.Compile();
            Renderer::ExecuteGraph(rg);
            Renderer::EndFrame();
            
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
                
                data.outputTex = rg.ImportResource(desc, 
                    (void*)static_cast<VKTexture*>(m_SceneColor.get())->GetImage(), 
                    (void*)static_cast<VKTexture*>(m_SceneColor.get())->GetImageView(), 
                    RG::ResourceState::Undefined);

                RG::TextureDesc depthDesc;
                depthDesc.name = "SceneDepth";
                depthDesc.width = m_SceneColor->GetWidth();
                depthDesc.height = m_SceneColor->GetHeight();
                depthDesc.format = RG::TextureFormat::D32_Float;

                data.depthTex = builder.CreateTexture(depthDesc);
                data.depthTex = builder.Write(data.depthTex);

                data.outputTex = builder.Write(data.outputTex);
                outputHandle = data.outputTex;
            },
            [this, &registry](GeometryPassData& data, RG::RenderPassContext& ctx)
            {
                VkCommandBuffer cmd = ctx.commandBuffer;
                RG::RenderGraph::ResourceNode* res = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.outputTex);
                RG::RenderGraph::ResourceNode* depthRes = (RG::RenderGraph::ResourceNode*)ctx.GetResource(data.depthTex);

                if (res)
                {
                    // Begin Rendering (Dynamic Rendering)
                    VkRenderingAttachmentInfo colorAttachment{};
                    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    colorAttachment.imageView = res->view;
                    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    colorAttachment.clearValue = { {{0.1f, 0.1f, 0.1f, 1.0f}} };

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

                    m_TrianglePipeline->Bind(cmd);

                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TrianglePipeline->GetLayout(), 0, 1, &m_GlobalDescriptorSet, 0, nullptr);

                    VkDescriptorSet bindlessSet = VulkanContext::Get().GetBindlessSet().GetSet();
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TrianglePipeline->GetLayout(), 1, 1, &bindlessSet, 0, nullptr);

                    VkViewport viewport{};
                    viewport.width = (float)res->desc.width;
                    viewport.height = (float)res->desc.height;
                    viewport.maxDepth = 1.0f;
                    vkCmdSetViewport(cmd, 0, 1, &viewport);

                    VkRect2D scissor{};
                    scissor.extent = { res->desc.width, res->desc.height };
                    vkCmdSetScissor(cmd, 0, 1, &scissor);

                    auto view = registry.view<Transform, MeshRenderer>();
                    for (auto [entity, transform, meshRenderer] : view.each())
                    {
                        auto model = AssetManager::GetAsset<Model>(meshRenderer.ModelUUID);
                        if (!model) continue;

                        auto mesh = model->GetMesh(meshRenderer.MeshIndex);
                        if (!mesh) continue;

                        auto vb = std::static_pointer_cast<VKVertexBuffer>(mesh->GetVertexBuffer());
                        auto ib = std::static_pointer_cast<VKIndexBuffer>(mesh->GetIndexBuffer());

                        if (!vb || !ib) continue;
                        
                        ObjectPushConstants pc{};
                        pc.modelMatrix = transform.GetTransform();
                        pc.albedoMapIndex = 0; // Default to 0 (white texture usually)

                        // Get Material
                        if (meshRenderer.MaterialUUID.IsValid())
                        {
                            auto material = AssetManager::GetAsset<Material>(meshRenderer.MaterialUUID);
                            if (material)
                            {
                                auto albedoTex = std::static_pointer_cast<VKTexture>(material->GetTextureByType(MapType::Diffuse));
                                if (albedoTex)
                                    pc.albedoMapIndex = albedoTex->GetBindlessIndex();
                            }
                        }

                        vkCmdPushConstants(cmd, m_TrianglePipeline->GetLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ObjectPushConstants), &pc);

                        VkBuffer vertexBuffers[] = { vb->GetVulkanBuffer() };
                        VkDeviceSize offsets[] = { 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

                        vkCmdBindIndexBuffer(cmd, ib->GetVulkanBuffer(), 0, VK_INDEX_TYPE_UINT32);

                        vkCmdDrawIndexed(cmd, ib->GetCount(), 1, 0, 0, 0);
                    }

                    vkCmdEndRendering(cmd);
                }
            }
        );
        return outputHandle;
    }

    void RenderingSystem::AddImGuiPass(RG::RenderGraph& rg, RG::ResourceHandle sceneColor)
    {
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

                if (sceneColor.IsValid())
                {
                    builder.Read(sceneColor);
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
    }

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        if (m_SceneColor && width > 0 && height > 0)
        {
            // Recreate texture
            m_SceneColor = Texture::Create(width, height, TextureFormat::RGBA8);
        }
    }
}
