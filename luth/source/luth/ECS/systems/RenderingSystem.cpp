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

#include <glad/glad.h>

namespace Luth
{
    RenderingSystem::RenderingSystem(u32 viewportWidth, u32 viewportHeight)
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

        // Register the “deferred” pipeline
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

    void RenderingSystem::Update(entt::registry& registry)
    {
        if (!m_ActivePipeline) return;

        // Collect opaque / transparent
        auto [opaque, transparent] = CollectCommands(registry);

        // Update camera / UBOs
        auto cam = Editor::GetPanel<ScenePanel>()->GetEditorCamera();
        m_CameraPos = cam.GetPosition();
        UpdateTransformUBO(cam.GetViewMatrix(), cam.GetProjectionMatrix(), Mat4(1.0f));
        UpdateLightsUBO(registry);

        // Build context and render
        RenderContext ctx{ m_ActivePipeline, registry, m_CameraPos, opaque, transparent,
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

    std::pair<std::vector<RenderCommand>, std::vector<RenderCommand>>
        RenderingSystem::CollectCommands(entt::registry& registry)
    {
        std::vector<RenderCommand> opaque, transparent;

        auto view = registry.view<Transform, MeshRenderer>();
        for (auto [entity, transform, meshRend] : view.each()) {
            auto material = MaterialLibrary::Get(meshRend.MaterialUUID);
            if (!material) material = MaterialLibrary::Get(UUID(7));

            RenderCommand cmd{
                .entity = entity,
                .transform = &transform,
                .meshRend = &meshRend,
                .distance = 0.0f
            };

            if (material->GetRenderMode() == RendererAPI::RenderMode::Opaque ||
                material->GetRenderMode() == RendererAPI::RenderMode::Cutout) {
                opaque.push_back(cmd);
            }
            else {
                // TODO: Calculate actual distance from camera
                //Vec3 worldPos = transform.GetWorldPosition();
                //cmd.distance = glm::distance(m_CameraPos, worldPos);
                Vec3 worldPos = Vec3(0.0f, 0.0f, 0.0f);
                cmd.distance = glm::distance(Vec3(400.0f, 220.0f, 400.0f), worldPos);
                transparent.push_back(cmd);
            }
        }

        // Sort transparent objects back-to-front
        std::sort(transparent.begin(), transparent.end(),
            [](const auto& a, const auto& b) { return a.distance > b.distance; });

        return { opaque, transparent };
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
