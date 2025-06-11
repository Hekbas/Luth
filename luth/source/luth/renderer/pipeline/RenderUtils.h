#pragma once

#include "luth/core/Log.h"
#include "luth/ECS/Entity.h"
#include "luth/renderer/Shader.h"
#include "luth/renderer/pipeline/RenderPass.h"
#include "luth/resources/libraries/ModelLibrary.h"
#include "luth/resources/libraries/MaterialLibrary.h"

namespace Luth::RenderUtils
{
    inline void BindMaterialTextures(const std::shared_ptr<Material>& material, Shader& shader)
    {
        int slot = 0;
        for (auto& texInfo : material->GetTextures()) {
            auto texture = TextureCache::Get(texInfo.TextureUuid);
            
            // Get appropriate default texture if needed
            if (!texture) {
                switch (texInfo.type) {
                    case MapType::Diffuse:   texture = TextureCache::GetDefaultWhite();  break;
                    case MapType::Alpha:     texture = TextureCache::GetDefaultWhite();  break;
                    case MapType::Normal:    texture = TextureCache::GetDefaultNormal(); break;
                    case MapType::Metalness: texture = TextureCache::GetDefaultGrey();   break;
                    case MapType::Roughness: texture = TextureCache::GetDefaultGrey();   break;
                    case MapType::Specular:  texture = TextureCache::GetDefaultGrey();   break;
                    case MapType::Oclusion:  texture = TextureCache::GetDefaultWhite();  break;
                    case MapType::Emissive:  texture = TextureCache::GetDefaultBlack();  break;
                    case MapType::Thickness: texture = TextureCache::GetDefaultBlack();  break;
                }
            }

            texture->Bind(slot);
            std::string prefix = "u_Maps[" + std::to_string((int)texInfo.type) + "]";
            shader.SetBool(prefix + ".useTexture", texInfo.useTexture);
            shader.SetInt(prefix + ".uvIndex", texInfo.uvIndex);
            shader.SetInt(prefix + ".texture", slot++);
        }

        // Material uniforms
        shader.SetVec4("u_Color",      material->GetColor());
        shader.SetFloat("u_Alpha",     material->GetAlpha());
        shader.SetFloat("u_Metalness", material->GetMetal());
        shader.SetFloat("u_Roughness", material->GetRough());
        shader.SetVec3("u_Emissive",   material->GetEmissive());

        shader.SetBool("u_IsGloss",         material->IsGloss());
        shader.SetBool("u_IsSingleChannel", material->IsSingleChannel());

        shader.SetVec3("u_Subsurface.color",           material->GetSubsurface().color);
        shader.SetFloat("u_Subsurface.strength",       material->GetSubsurface().strength);
        shader.SetFloat("u_Subsurface.thicknessScale", material->GetSubsurface().thicknessScale);
    }

    inline void RenderMesh(const RenderCommand& cmd, Shader& shader)
    {
        auto& meshRend = *cmd.meshRend;
        auto model = ModelLibrary::Get(meshRend.ModelUUID);
        if (!model) {
            LH_CORE_WARN("MeshRenderer missing model reference");
            return;
        }

        auto& meshes = model->GetMeshes();
        if (meshRend.MeshIndex >= meshes.size()) {
            LH_CORE_ERROR("Invalid mesh index: {0}", meshRend.MeshIndex);
            return;
        }

        shader.SetMat4("u_Model", cmd.transform->matrix);
        meshes[meshRend.MeshIndex]->Draw();
    }

    inline void DrawCommand(const RenderCommand& cmd, Shader& shader, bool bindMaterial = true)
    {
        std::shared_ptr<Material> material;
        if (bindMaterial) {
            material = MaterialLibrary::Get(cmd.meshRend->MaterialUUID);
            if (!material) material = MaterialLibrary::Get(UUID(7)); // fallback
        }

        shader.Bind();

        if (bindMaterial && material) {
            // per-material modes
            shader.SetInt("u_RenderMode", static_cast<int>(material->GetRenderMode()));

            if (material->GetRenderMode() == RendererAPI::RenderMode::Cutout) {
                shader.SetFloat("u_AlphaCutoff", material->GetAlphaCutoff());
            }
            else if (material->GetRenderMode() == RendererAPI::RenderMode::Transparent) {
                shader.SetBool("u_AlphaFromDiffuse", material->IsAlphaFromDiffuseEnabled());
                shader.SetFloat("u_Alpha", material->GetAlpha());
            }

            // Uniforms & Textures
            BindMaterialTextures(material, shader);
        }

        RenderMesh(cmd, shader);
    }
}
