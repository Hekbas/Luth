#include "luthpch.h"
#include "luth/ECS/systems/RenderingSystem.h"
#include "luth/renderer/pipeline/passes/GeometryPass.h"
#include "luth/renderer/pipeline/passes/SSAOPass.h"
#include "luth/renderer/pipeline/passes/LightingPass.h"
#include "luth/renderer/pipeline/passes/TransparentPass.h"
#include "luth/renderer/pipeline/passes/PostProcessPass.h"
#include "luth/resources/libraries/MaterialLibrary.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Profiler.h"
#include "luth/graphics/GfxRenderer.h" // New Renderer
#include "luth/renderer/vulkan/VKResourceManager.h" // Need definition of VKImageResource

#include <glad/glad.h>

namespace Luth
{
    RenderingSystem::RenderingSystem(u32 viewportWidth, u32 viewportHeight)
    {
        // Initialize Frame Allocator (1MB should be enough for command lists for now)
        m_FrameAllocator = std::make_unique<LinearAllocator>(1 * Memory::MB);

        if (Renderer::GetAPI() == RendererAPI::API::OpenGL)
        {
            // UBO setup
            glGenBuffers(1, &m_TransformUBO);
            glBindBuffer(GL_UNIFORM_BUFFER, m_TransformUBO);
            glBufferData(GL_UNIFORM_BUFFER, sizeof(TransformUBO), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_TransformUBO);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);

            glGenBuffers(1, &m_LightsUBO);
            glBindBuffer(GL_UNIFORM_BUFFER, m_LightsUBO);
            glBufferData(GL_UNIFORM_BUFFER, sizeof(LightsUBO), nullptr, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_LightsUBO);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);

            // Register the deferred pipeline
            RenderPipeline deferredPipeline;
            deferredPipeline.AddPass<GeometryPass>();
            deferredPipeline.AddPass<SSAOPass>();
            deferredPipeline.AddPass<LightingPass>();
            deferredPipeline.AddPass<TransparentPass>();
            deferredPipeline.AddPass<PostProcessPass>();
            deferredPipeline.InitAll(viewportWidth, viewportHeight);

            RegisterTechnique("Forward", std::move(deferredPipeline));
            SetActiveTechnique("Forward");
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
            Gfx::GfxRenderer::BeginFrame();

            RG::RenderGraph rg(*m_FrameAllocator);

            struct GeometryPassData {
                RG::ResourceHandle outputTex;
            };

            // 1. Geometry Pass (Draws to SceneColor)
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
                },
                [&](GeometryPassData& data, RG::RenderPassContext& ctx)
                {
                    // LH_CORE_INFO("Executing Geometry Pass");
                    // Here we would bind the framebuffer and draw meshes
                }
            );

            // 2. Present Pass (Copies SceneColor to Backbuffer)
            // For now, we just clear the backbuffer to prove we can write to it.
            struct PresentPassData {
                RG::ResourceHandle backbuffer;
            };

            rg.AddPass<PresentPassData>("PresentPass",
                [&](PresentPassData& data, RG::RenderPassBuilder& builder)
                {
                    // HACK: We will create a "Virtual Backbuffer" here.
                    RG::TextureDesc desc;
                    desc.name = "Backbuffer";
                    desc.width = 1280;
                    desc.height = 720;
                    desc.format = RG::TextureFormat::RGBA8_Unorm;
                    
                    // We register it as a normal resource for now, 
                    // and VKRendererAPI will overwrite its physical pointer.
                    data.backbuffer = builder.CreateTexture(desc);
                    
                    // Use WriteTransfer because we use vkCmdClearColorImage
                    data.backbuffer = builder.WriteTransfer(data.backbuffer);
                },
                [&](PresentPassData& data, RG::RenderPassContext& ctx)
                {
                    VkCommandBuffer cmd = (VkCommandBuffer)ctx.commandBuffer;
                    VKImageResource* res = (VKImageResource*)ctx.GetResource(data.backbuffer);
                    
                    if (res)
                    {
                        // Clear to Magenta
                        VkClearColorValue clearColor = { { 1.0f, 0.0f, 1.0f, 1.0f } };
                        
                        VkImageSubresourceRange range{};
                        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        range.baseMipLevel = 0;
                        range.levelCount = 1;
                        range.baseArrayLayer = 0;
                        range.layerCount = 1;

                        vkCmdClearColorImage(cmd, res->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);
                    }
                }
            );

            rg.Compile();
            Gfx::GfxRenderer::ExecuteGraph(rg);
            Gfx::GfxRenderer::EndFrame();
            
            return;
        }
        // -----------------------------------------------------------------

        if (!m_ActivePipeline) return;

        // Collect opaque / transparent
        auto [opaque, transparent] = CollectCommands(registry);

        // Update camera / UBOs
        auto cam = Editor::GetPanel<ScenePanel>()->GetEditorCamera();
        m_CameraPos = cam.GetPosition();
        UpdateTransformUBO(cam.GetViewMatrix(), cam.GetProjectionMatrix(), Mat4(1.0f));
        UpdateLightsUBO(registry);

        // Build context and render
        // Note: We need to convert spans back to vectors for the old pipeline API for now
        // Ideally RenderPipeline should accept spans.
        std::vector<RenderCommand> opaqueVec(opaque.begin(), opaque.end());
        std::vector<RenderCommand> transparentVec(transparent.begin(), transparent.end());

        RenderContext ctx{ m_ActivePipeline, registry, m_CameraPos, opaqueVec, transparentVec,
                            (u32)m_ViewProj[0][0], (u32)m_ViewProj[1][1] };
        m_ActivePipeline->RenderAll(ctx);
    }

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        if (m_ActivePipeline)
            m_ActivePipeline->ResizeAll(width, height);
    }

    void RenderingSystem::RegisterTechnique(const std::string& name, RenderPipeline&& pipeline)
    {
        m_Pipelines[name] = std::move(pipeline);
    }

    void RenderingSystem::SetActiveTechnique(const std::string& name)
    {
        auto it = m_Pipelines.find(name);
        if (it == m_Pipelines.end()) return;
        m_ActivePipeline = &it->second;
        m_ActiveName = it->first;
    }

    std::vector<std::string> RenderingSystem::GetTechniqueNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_Pipelines.size());
        for (auto& [n, _] : m_Pipelines) names.push_back(n);
        return names;
    }

    const std::string& RenderingSystem::GetActiveTechniqueName() const
    {
        return m_ActiveName;
    }

    struct CollectCommandsData
    {
        entt::registry* registry;
        std::vector<entt::entity>* entities;
        RenderCommand* opaqueCmds;
        RenderCommand* transparentCmds;
        std::atomic<u32>* opaqueCount;
        std::atomic<u32>* transparentCount;
    };

    static void CollectCommandsJob(JobSystem::JobArgs args)
    {
        CollectCommandsData* data = (CollectCommandsData*)args.data;
        entt::registry& registry = *data->registry;
        entt::entity entity = (*data->entities)[args.jobIndex];

        // We need to get components manually since we can't capture the view
        auto& transform = registry.get<WorldTransform>(entity);
        auto& meshRend = registry.get<MeshRenderer>(entity);

        // Material lookup (Thread safe? MaterialLibrary::Get needs to be safe!)
        auto material = MaterialLibrary::Get(meshRend.MaterialUUID);
        if (!material) material = MaterialLibrary::Get(UUID(7));

        RenderCommand cmd{
            .entity = entity,
            .transform = &transform,
            .meshRend = &meshRend,
            .distance = 0.0f
        };

        if (material->GetRenderMode() == RendererAPI::RenderMode::Opaque ||
            material->GetRenderMode() == RendererAPI::RenderMode::Cutout) 
        {
            u32 index = data->opaqueCount->fetch_add(1);
            data->opaqueCmds[index] = cmd;
        }
        else 
        {
            // Placeholder distance logic
            Vec3 worldPos = Vec3(0.0f); 
            cmd.distance = glm::distance(Vec3(400.0f, 220.0f, 400.0f), worldPos);
            
            u32 index = data->transparentCount->fetch_add(1);
            data->transparentCmds[index] = cmd;
        }
    }

    std::pair<std::span<RenderCommand>, std::span<RenderCommand>>
        RenderingSystem::CollectCommands(entt::registry& registry)
    {
        LH_PROFILE_SCOPE("CollectCommands");

        auto view = registry.view<WorldTransform, MeshRenderer>();
        u32 entityCount = (u32)view.size_hint();

        // Allocate worst-case memory from LinearAllocator (fast!)
        RenderCommand* commands = (RenderCommand*)m_FrameAllocator->Allocate(sizeof(RenderCommand) * entityCount);
        
        // Atomic counters for parallel filling
        std::atomic<u32> opaqueCount = 0;
        std::atomic<u32> transparentCount = 0;

        RenderCommand* opaqueCmds = commands;
        RenderCommand* transparentCmds = (RenderCommand*)m_FrameAllocator->Allocate(sizeof(RenderCommand) * entityCount);

        // Convert view to vector for dispatch
        std::vector<entt::entity> entities;
        entities.reserve(entityCount);
        for (auto entity : view) entities.push_back(entity);

        if (entities.empty())
        {
            return { std::span<RenderCommand>(), std::span<RenderCommand>() };
        }

        CollectCommandsData jobData;
        jobData.registry = &registry;
        jobData.entities = &entities;
        jobData.opaqueCmds = opaqueCmds;
        jobData.transparentCmds = transparentCmds;
        jobData.opaqueCount = &opaqueCount;
        jobData.transparentCount = &transparentCount;

        JobSystem::Counter counter;
        JobSystem::Dispatch((u32)entities.size(), 64, CollectCommandsJob, &jobData, &counter);
        JobSystem::WaitForCounter(&counter);

        // Sort transparent objects (Serial for now, can be parallelized with parallel_sort)
        {
            LH_PROFILE_SCOPE("SortTransparent");
            std::sort(transparentCmds, transparentCmds + transparentCount,
                [](const auto& a, const auto& b) { return a.distance > b.distance; });
        }

        return { 
            std::span<RenderCommand>(opaqueCmds, opaqueCount), 
            std::span<RenderCommand>(transparentCmds, transparentCount) 
        };
    }

    void RenderingSystem::UpdateTransformUBO(const Mat4& view, const Mat4& proj, const Mat4& model)
    {
        TransformUBO data{ view, proj, model };
        glBindBuffer(GL_UNIFORM_BUFFER, m_TransformUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(data), &data);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    void RenderingSystem::UpdateLightsUBO(entt::registry& registry)
    {
        LightsUBO ubo{};
        ubo.dirLightCount = 0;
        ubo.pointLightCount = 0;

        // Process directional lights
        auto dirLightsView = registry.view<DirectionalLight, Transform>();
        for (auto [entity, dirLight, transform] : dirLightsView.each()) {
            if (ubo.dirLightCount >= MAX_DIR_LIGHTS) break;

            ubo.dirLights[ubo.dirLightCount] = {
                .color = dirLight.Color,
                .intensity = dirLight.Intensity,
                .direction = transform.m_Rotation,
                .padding = 0.0f
            };
            ubo.dirLightCount++;
        }

        // Process point lights with their transforms
        auto pointLightsView = registry.view<PointLight, Transform>();
        for (auto [entity, pointLight, transform] : pointLightsView.each()) {
            if (ubo.pointLightCount >= MAX_POINT_LIGHTS) break;

            ubo.pointLights[ubo.pointLightCount] = {
                .color = pointLight.Color,
                .intensity = pointLight.Intensity,
                .position = transform.m_Position,
                .range = pointLight.Range
            };
            ubo.pointLightCount++;
        }

        // Update GPU buffer
        glBindBuffer(GL_UNIFORM_BUFFER, m_LightsUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(LightsUBO), &ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
}
