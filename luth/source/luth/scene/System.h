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

                if (!model || !material) {
                    //LH_CORE_WARN("Missing resources for MeshRenderer");
                    return;
                }

                // Validate mesh index
                const auto& meshes = model->GetMeshes();
                if (meshRend.MeshIndex >= meshes.size()) {
                    LH_CORE_ERROR("Invalid mesh index: {0}", meshRend.MeshIndex);
                    return;
                }

                // Get shader and setup transform
                auto shader = material->GetShader();
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

    private:

        void BindMaterialTextures(const std::shared_ptr<Material>& material,
            const std::shared_ptr<Shader>& shader) {
            int slot = 0;
            for (const auto& texInfo : material->GetTextures()) {
                auto texture = TextureCache::Get(texInfo.Uuid);
                if (!texture) continue;

                texture->Bind(slot);

                // Set texture uniforms based on type
                switch (texInfo.type) {
                    case TextureType::Diffuse:
                        shader->SetInt("u_TexDiffuse", slot);
                        shader->SetInt("u_UVIndexDiffuse", texInfo.uvIndex);
                        break;
                    case TextureType::Normal:
                        shader->SetInt("u_TexNormal", slot);
                        shader->SetInt("u_UVIndexNormal", texInfo.uvIndex);
                        break;
                    case TextureType::Emissive:
                        shader->SetInt("u_TexEmissive", slot);
                        shader->SetInt("u_UVIndexEmissive", texInfo.uvIndex);
                        break;
                    case TextureType::Metalness:
                        shader->SetInt("u_TexMetallic", slot);
                        shader->SetInt("u_UVIndexMetallic", texInfo.uvIndex);
                        break;
                    case TextureType::Roughness:
                        shader->SetInt("u_TexRoughness", slot);
                        shader->SetInt("u_UVIndexRoughness", texInfo.uvIndex);
                        break;
                    case TextureType::Specular:
                        shader->SetInt("u_TexSpecular", slot);
                        shader->SetInt("u_UVIndexSpecular", texInfo.uvIndex);
                        break;
                    default: LH_CORE_ERROR("TextureType not supported!");
                }

                slot++;
            }
        }

        glm::mat4 CalculateModelMatrix(const Transform& t) {
            // TODO: Create a matrix from transform components
            return glm::mat4(1.0f);
        }
    };
}
