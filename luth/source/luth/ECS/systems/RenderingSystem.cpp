#include "luthpch.h"
#include "luth/ECS/systems/RenderingSystem.h"
#include "luth/renderer/techniques/ForwardTechnique.h"
#include "luth/renderer/techniques/DeferredTechnique.h"
#include "luth/resources/libraries/MaterialLibrary.h"

#include <entt/entt.hpp>

namespace Luth
{
    RenderingSystem::RenderingSystem()
    {
        // Default to forward rendering
        m_ActiveTechnique = std::make_shared<DeferredTechnique>();
        m_ActiveTechnique->Init(1280, 720);
    }

    void RenderingSystem::Update(entt::registry& registry)
    {
        auto [opaque, transparent] = CollectCommands(registry);

        if (m_ActiveTechnique) {
            m_ActiveTechnique->Render(registry, m_CameraPos, opaque, transparent);
        }
    }

    void RenderingSystem::SetTechnique(std::shared_ptr<RenderTechnique> technique)
    {
        if (m_ActiveTechnique) m_ActiveTechnique->Shutdown();
        m_ActiveTechnique = technique;
        if (m_ActiveTechnique) {
            m_ActiveTechnique->Init(m_ActiveTechnique->GetWidth(),
                m_ActiveTechnique->GetHeight());
        }
    }

    void RenderingSystem::Resize(u32 width, u32 height)
    {
        if (m_ActiveTechnique) {
            m_ActiveTechnique->Resize(width, height);
        }
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


    //void Update(entt::registry& registry) override {

    //    // TODO: Implement actual camera position
    //    //auto cameraPos = GetActiveCameraPosition();
    //    auto cameraPos = Vec3(40.0f, 20.0f, 40.0f);

    //    std::vector<RenderCommand> opaqueCommands, transparentCommands;

    //    // Collect entities
    //    auto view = registry.view<Transform, MeshRenderer>();

    //    for (auto [entity, transform, meshRend] : view.each()) {
    //        auto material = MaterialLibrary::Get(meshRend.MaterialUUID);
    //        if (!material) material = MaterialLibrary::Get(UUID(7));

    //        RenderCommand cmd{ entity, &transform, &meshRend };

    //        if (material->GetRenderMode() == RendererAPI::RenderMode::Opaque ||
    //            material->GetRenderMode() == RendererAPI::RenderMode::Cutout) {
    //            opaqueCommands.push_back(cmd);
    //        }
    //        else {
    //            // TODO: Calculate actual distance from camera
    //            //Vec3 pos = transform.GetWorldPosition();
    //            Vec3 pos = Vec3(0.0f, 0.0f, 0.0f);
    //            cmd.distance = glm::distance(cameraPos, pos);
    //            transparentCommands.push_back(cmd);
    //        }
    //    }

    //    // Sort transparent commands back-to-front
    //    std::sort(transparentCommands.begin(), transparentCommands.end(),
    //        [](const auto& a, const auto& b) { return a.distance > b.distance; });

    //    // Render opaque objects
    //    for (const auto& cmd : opaqueCommands) {
    //        RenderMesh(*cmd.transform, *cmd.meshRend, true);
    //    }

    //    // Render transparent objects
    //    for (const auto& cmd : transparentCommands) {
    //        RenderMesh(*cmd.transform, *cmd.meshRend, false);
    //    }
    //}

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
