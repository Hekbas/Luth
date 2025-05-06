#include "Luthpch.h"
#include "luth/renderer/techniques/ForwardTechnique.h"
#include "luth/resources/libraries/MaterialLibrary.h"
#include "luth/resources/libraries/ModelLibrary.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"

namespace Luth
{
    void ForwardTechnique::Init(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;

        Framebuffer::Spec spec{
            .Width = width,
            .Height = height,
            .ColorAttachments = {
                {.InternalFormat = GL_RGBA16F},
            },
            .DepthStencilAttachment = {
                {.InternalFormat = GL_DEPTH24_STENCIL8, .IsTexture = false}
            }
        };
        m_MainFBO = Framebuffer::Create(spec);
    }

    void ForwardTechnique::Shutdown()
    {
        m_MainFBO.reset();
    }

    void ForwardTechnique::Render(entt::registry& registry,
        const Vec3& cameraPos,
        const std::vector<RenderCommand>& opaque,
        const std::vector<RenderCommand>& transparent)
    {
        m_MainFBO->Bind();
        Renderer::Clear(/*GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT*/);

        // Opaque objects
        Renderer::EnableBlending(false);
        for (const auto& cmd : opaque) {
            RenderMesh(cmd, true);
        }

        // Transparent objects
        Renderer::EnableBlending(true);
        Renderer::EnableDepthMask(false);
        for (const auto& cmd : transparent) {
            RenderMesh(cmd, false);
        }
        Renderer::EnableDepthMask(true);

        m_MainFBO->Unbind();
    }

    void ForwardTechnique::RenderMesh(const RenderCommand& cmd, bool isOpaque)
    {
        auto& transform = *cmd.transform;
        auto& meshRend = *cmd.meshRend;

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

        auto shader = material->GetShader();
        if (!shader) {
            LH_CORE_WARN("Invalid shader for material");
            return;
        }

        shader->Bind();
        //shader->SetMat4("u_Model", transform.GetTransform());

        // Material properties
        shader->SetInt("u_RenderMode", static_cast<int>(material->GetRenderMode()));

        if (material->GetRenderMode() == RendererAPI::RenderMode::Cutout) {
            shader->SetFloat("u_AlphaCutoff", material->GetAlphaCutoff());
        }
        else if (material->GetRenderMode() == RendererAPI::RenderMode::Transparent) {
            shader->SetBool("u_AlphaFromDiffuse", material->IsAlphaFromDiffuseEnabled());
            shader->SetFloat("u_Alpha", material->GetAlpha());
        }

        BindMaterialTextures(material, shader);
        model->GetMeshes()[meshRend.MeshIndex]->Draw();
    }

    void ForwardTechnique::BindMaterialTextures(const std::shared_ptr<Material>& material,
        const std::shared_ptr<Shader>& shader)
    {
        int slot = 0;
        for (const auto& texInfo : material->GetTextures()) {
            auto texture = TextureCache::Get(texInfo.TextureUuid);
            const int mapIndex = static_cast<int>(texInfo.type);

            // Get appropriate default texture if needed
            if (!texture) {
                switch (texInfo.type) {
                    case MapType::Diffuse:      texture = TextureCache::GetDefaultWhite();  break;
                    case MapType::Alpha:        texture = TextureCache::GetDefaultWhite();  break;
                    case MapType::Normal:       texture = TextureCache::GetDefaultNormal(); break;
                    case MapType::Metalness:    texture = TextureCache::GetDefaultGrey();   break;
                    case MapType::Roughness:    texture = TextureCache::GetDefaultGrey();   break;
                    case MapType::Specular:     texture = TextureCache::GetDefaultGrey();   break;
                    case MapType::Oclusion:     texture = TextureCache::GetDefaultWhite();  break;
                    case MapType::Emissive:     texture = TextureCache::GetDefaultBlack();  break;
                }
            }

            // Bind texture to slot
            texture->Bind(slot);

            // Set struct properties
            const std::string prefix = "u_Maps[" + std::to_string(mapIndex) + "]";
            shader->SetBool(prefix + ".useTexture", texInfo.useTexture);
            shader->SetInt(prefix + ".uvIndex", texInfo.uvIndex);
            shader->SetInt(prefix + ".texture", slot);

            slot++;
        }

        // Set material uniforms
        shader->SetVec4("u_Color", material->GetColor());
        shader->SetFloat("u_Alpha", material->GetAlpha());
        shader->SetFloat("u_Metalness", material->GetMetal());
        shader->SetFloat("u_Roughness", material->GetRough());
        shader->SetVec3("u_Emissive", material->GetEmissive());
        shader->SetBool("u_IsGloss", material->IsGloss());
        shader->SetBool("u_IsSingleChannel", material->IsSingleChannel());
    }

    void ForwardTechnique::Resize(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;
        m_MainFBO->Resize(width, height);
    }

    std::vector<std::pair<std::string, u32>> ForwardTechnique::GetAllAttachments() const
    {
        return m_MainFBO->GetAllAttachments();
    }
}
