#include "luthpch.h"
#include "luth/ECS/systems/RenderingSystem.h"
#include "luth/renderer/techniques/ForwardTechnique.h"
#include "luth/renderer/techniques/DeferredTechnique.h"
#include "luth/resources/libraries/MaterialLibrary.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"

#include <entt/entt.hpp>

namespace Luth
{
    RenderingSystem::RenderingSystem()
    {
        RegisterMainTechnique("Forward", std::make_shared<ForwardTechnique>());
        RegisterMainTechnique("Deferred", std::make_shared<DeferredTechnique>());

        // TODO: Manage UBOs through Renderer, make agnostic
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
    }

    void RenderingSystem::Update(entt::registry& registry)
    {
        auto [opaque, transparent] = CollectCommands(registry);

        if (m_ActiveTechnique) {
            EditorCamera cam = Editor::GetPanel<ScenePanel>()->GetEditorCamera();
            UpdateTransformUBO(cam.GetViewMatrix(), cam.GetProjectionMatrix(), Mat4(1.0f));
            UpdateLightsUBO(registry);
            m_ActiveTechnique->Render(registry, m_CameraPos, opaque, transparent);
        }
    }

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        if (m_ActiveTechnique) {
            m_ActiveTechnique->Resize(width, height);
        }
    }

    void RenderingSystem::RegisterMainTechnique(const std::string& name, std::shared_ptr<RenderTechnique> technique)
    {
        m_MainTechniques[name] = technique;
        if (!m_ActiveTechnique) { // Auto-select first technique
            m_ActiveTechnique = technique;
        }
    }

    void RenderingSystem::SetTechnique(const std::string& name)
    {
        if (auto it = m_MainTechniques.find(name); it != m_MainTechniques.end()) {
            u32 width = m_ActiveTechnique->GetWidth();
            u32 height = m_ActiveTechnique->GetHeight();
            m_ActiveTechnique = it->second;
            m_ActiveTechnique->Resize(width, height);
            //m_ActiveTechnique->SetViewProjection(m_ViewProjection);
        }
    }

    void RenderingSystem::SetViewProjection(const Mat4& vp)
    {
        /*m_ViewProjection = vp;
        if (m_ActiveTechnique) {
            m_ActiveTechnique->SetViewProjection(vp);
        }*/
    }

    std::pair<std::vector<RenderCommand>, std::vector<RenderCommand>>
        RenderingSystem::CollectCommands(entt::registry& registry)
    {
        std::vector<RenderCommand> opaqueCommands;
        std::vector<RenderCommand> transparentCommands;

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
                opaqueCommands.push_back(cmd);
            }
            else {
                // TODO: Calculate actual distance from camera
                //Vec3 worldPos = transform.GetWorldPosition();
                //cmd.distance = glm::distance(m_CameraPos, worldPos);
                Vec3 worldPos = Vec3(0.0f, 0.0f, 0.0f);
                cmd.distance = glm::distance(Vec3(400.0f, 220.0f, 400.0f), worldPos);
                transparentCommands.push_back(cmd);
            }
        }

        // Sort transparent objects back-to-front
        std::sort(transparentCommands.begin(), transparentCommands.end(),
            [](const auto& a, const auto& b) { return a.distance > b.distance; });

        return { opaqueCommands, transparentCommands };
    }

    void RenderingSystem::UpdateTransformUBO(const Mat4& view, const Mat4& projection, const Mat4& model)
    {
        TransformUBO data;
        data.view = view;
        data.projection = projection;
        data.model = model;

        glBindBuffer(GL_UNIFORM_BUFFER, m_TransformUBO);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(TransformUBO), &data);
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

    //void RenderMesh(Transform& transform, MeshRenderer& meshRend, bool isOpaque) {
    //    // Get resources from UUIDs
    //    auto model = ModelLibrary::Get(meshRend.ModelUUID);
    //    auto material = MaterialLibrary::Get(meshRend.MaterialUUID);

    //    if (!model) {
    //        LH_CORE_WARN("MeshRenderer missing model reference");
    //        return;
    //    }

    //    // Validate mesh index
    //    const auto& meshes = model->GetMeshes();
    //    if (meshRend.MeshIndex >= meshes.size()) {
    //        LH_CORE_ERROR("Invalid mesh index: {0}", meshRend.MeshIndex);
    //        return;
    //    }

    //    if (!material) material = MaterialLibrary::Get(UUID(7));

    //    // Get shader and setup transform
    //    auto shader = m_Override ? m_ShaderOverride : material->GetShader();
    //    if (!shader) {
    //        LH_CORE_WARN("Invalid shader for material");
    //        return;
    //    }

    //    // Set shader uniforms
    //    shader->Bind();
    //    //shader->SetMat4("u_Model", transform.GetTransform());
    //    shader->SetInt("u_RenderMode", static_cast<int>(material->GetRenderMode()));

    //    if (material->GetRenderMode() == RendererAPI::RenderMode::Cutout) {
    //        shader->SetFloat("u_AlphaCutoff", material->GetAlphaCutoff());
    //    }
    //    if (material->GetRenderMode() == RendererAPI::RenderMode::Transparent) {
    //        shader->SetBool("u_AlphaFromDiffuse", material->IsAlphaFromDiffuseEnabled());
    //        shader->SetFloat("u_Alpha", material->GetAlpha());
    //    }
    //    shader->SetVec4("u_Color", material->GetColor());

    //    // Configure render states
    //    if (isOpaque) {
    //        Renderer::EnableBlending(false);
    //    }
    //    else {
    //        Renderer::EnableBlending(true);
    //        Renderer::SetBlendFunction(material->GetBlendSrc(), material->GetBlendDst());
    //    }

    //    // Bind textures and draw
    //    BindMaterialTextures(material, shader);
    //    meshes[meshRend.MeshIndex]->Draw();
    //}

    //void SetShaderOverride(bool override, UUID uuid) {
    //    m_Override = override;
    //    m_ShaderOverride = ShaderLibrary::Get(uuid);
    //}

    //    // Shader controls
    //    std::shared_ptr<Shader> m_ShaderOverride;
    //    bool m_Override = false;
}
