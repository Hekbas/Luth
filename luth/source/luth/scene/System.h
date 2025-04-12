#pragma once

#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/Material.h"
#include "luth/resources/libraries/ModelLibrary.h"
#include "luth/resources/libraries/MaterialLibrary.h"

#include <entt/entt.hpp>

namespace Luth
{
    class System
    {
    public:
        virtual ~System() = default;
        virtual void Update(entt::registry& registry) = 0;
    };

    class RenderingSystem : public System
    {
    public:
        void Update(entt::registry& registry) override {
            struct RenderCommand {
                entt::entity entity;
                Transform* transform;
                MeshRenderer* meshRend;
                float distance;
            };

            // TODO: Implement actual camera position
            //auto cameraPos = GetActiveCameraPosition();
            auto cameraPos = Vec3(40.0f, 20.0f, 40.0f);

            std::vector<RenderCommand> opaqueCommands, transparentCommands;

            // Collect entities
            auto view = registry.view<Transform, MeshRenderer>();

            for (auto [entity, transform, meshRend] : view.each()) {
                auto material = MaterialLibrary::Get(meshRend.MaterialUUID);
                if (!material) material = MaterialLibrary::Get(UUID(7));

                RenderCommand cmd{ entity, &transform, &meshRend };

                if (material->GetRenderMode() == RendererAPI::RenderMode::Opaque ||
                    material->GetRenderMode() == RendererAPI::RenderMode::Cutout) {
                    opaqueCommands.push_back(cmd);
                }
                else {
                    // Calculate distance from camera
                    //Vec3 pos = transform.GetWorldPosition();
                    Vec3 pos = Vec3(0.0f, 0.0f, 0.0f);
                    cmd.distance = glm::distance(cameraPos, pos);
                    transparentCommands.push_back(cmd);
                }
            }

            // Sort transparent commands back-to-front
            std::sort(transparentCommands.begin(), transparentCommands.end(),
                [](const auto& a, const auto& b) { return a.distance > b.distance; });

            // Render opaque objects
            for (const auto& cmd : opaqueCommands) {
                RenderMesh(*cmd.transform, *cmd.meshRend, true);
            }

            // Render transparent objects
            for (const auto& cmd : transparentCommands) {
                RenderMesh(*cmd.transform, *cmd.meshRend, false);
            }
        }

        void RenderMesh(Transform& transform, MeshRenderer& meshRend, bool isOpaque) {
            // Get resources from UUIDs
            auto model = ModelLibrary::Get(meshRend.ModelUUID);
            auto material = MaterialLibrary::Get(meshRend.MaterialUUID);

            if (!model) {
                LH_CORE_WARN("MeshRenderer missing model reference");
                return;
            }

            // Validate mesh index
            const auto& meshes = model->GetMeshes();
            if (meshRend.MeshIndex >= meshes.size()) {
                LH_CORE_ERROR("Invalid mesh index: {0}", meshRend.MeshIndex);
                return;
            }

            if (!material) material = MaterialLibrary::Get(UUID(7));

            // Get shader and setup transform
            auto shader = m_Override ? m_ShaderOverride : material->GetShader();
            if (!shader) {
                LH_CORE_WARN("Invalid shader for material");
                return;
            }

            // Set shader uniforms
            shader->Bind();
            //shader->SetMat4("u_Model", transform.GetTransform());
            shader->SetInt("u_RenderMode", static_cast<int>(material->GetRenderMode()));

            // Handle Cutoff
            if (material->GetRenderMode() == RendererAPI::RenderMode::Cutout) {
                shader->SetFloat("u_AlphaCutoff", material->GetAlphaCutoff());
            }

            // Configure render states
            if (isOpaque) {
                Renderer::EnableBlending(false);
            }
            else {
                Renderer::EnableBlending(true);
                Renderer::SetBlendFunction(material->GetBlendSrc(), material->GetBlendDst());
            }

            // Bind textures and draw
            BindMaterialTextures(material, shader);
            meshes[meshRend.MeshIndex]->Draw();
        }

        void SetShaderOverride(bool override, UUID uuid) { 
            m_Override = override;
            m_ShaderOverride = ShaderLibrary::Get(uuid);
        }

    private:

        void BindMaterialTextures(const std::shared_ptr<Material>& material,
            const std::shared_ptr<Shader>& shader) {

            int slot = 0;
            for (const auto& texInfo : material->GetTextures()) {
                auto texture = TextureCache::Get(texInfo.Uuid);

                // Set texture uniforms based on type
                switch (texInfo.type) {
                    case TextureType::Diffuse:
                        if (!texture) texture = TextureCache::GetDefaultWhite();
                        shader->SetInt("u_TexDiffuse", slot);
                        shader->SetInt("u_UVIndexDiffuse", texInfo.uvIndex);
                        break;
                    case TextureType::Alpha:
                        if (!texture) texture = TextureCache::GetDefaultWhite();
                        shader->SetInt("u_TexAlpha", slot);
                        shader->SetInt("u_UVIndexAlpha", texInfo.uvIndex);
                        break;
                    case TextureType::Normal:
                        if (!texture) texture = TextureCache::GetDefaultGrey();
                        shader->SetInt("u_TexNormal", slot);
                        shader->SetInt("u_UVIndexNormal", texInfo.uvIndex);
                        break;
                    case TextureType::Emissive:
                        if (!texture) texture = TextureCache::GetDefaultBlack();
                        shader->SetInt("u_TexEmissive", slot);
                        shader->SetInt("u_UVIndexEmissive", texInfo.uvIndex);
                        break;
                    case TextureType::Metalness:
                        if (!texture) texture = TextureCache::GetDefaultGrey();
                        shader->SetInt("u_TexMetallic", slot);
                        shader->SetInt("u_UVIndexMetallic", texInfo.uvIndex);
                        break;
                    case TextureType::Roughness:
                        if (!texture) texture = TextureCache::GetDefaultGrey();
                        shader->SetInt("u_TexRoughness", slot);
                        shader->SetInt("u_UVIndexRoughness", texInfo.uvIndex);
                        break;
                    case TextureType::Specular:
                        if (!texture) texture = TextureCache::GetDefaultGrey();
                        shader->SetInt("u_TexSpecular", slot);
                        shader->SetInt("u_UVIndexSpecular", texInfo.uvIndex);
                        break;
                    case TextureType::Oclusion:
                        if (!texture) texture = TextureCache::GetDefaultWhite();
                        shader->SetInt("u_TexOclusion", slot);
                        shader->SetInt("u_UVIndexOclusion", texInfo.uvIndex);
                        break;
                    default: LH_CORE_ERROR("TextureType not supported!");
                }

                texture->Bind(slot);
                slot++;
            }
        }

        glm::mat4 CalculateModelMatrix(const Transform& t) {
            // TODO: Create a matrix from transform components
            return glm::mat4(1.0f);
        }

        // Shader controls
        std::shared_ptr<Shader> m_ShaderOverride;
        bool m_Override = false;
    };
}
