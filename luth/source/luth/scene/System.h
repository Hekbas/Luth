#pragma once

#include "luth/scene/Components.h"
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
            auto view = registry.view<Transform, MeshRenderer>();

            view.each([this](auto entity, Transform& transform, MeshRenderer& meshRend) {
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

                //const glm::mat4 modelMatrix = CalculateModelMatrix(transform);
                shader->Bind();
                //shader->SetMat4("u_Model", transform.GetTransform());

                // Bind material properties
                BindMaterialTextures(material, shader);

                // Draw the mesh
                meshes[meshRend.MeshIndex]->Draw();
            });
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
