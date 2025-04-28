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
        //Renderer::EnableDepthTest(false);
        for (const auto& cmd : transparent) {
            RenderMesh(cmd, false);
        }
        //Renderer::EnableDepthTest(true);

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
        shader->SetVec4("u_Color", material->GetColor());

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
            auto texture = TextureCache::Get(texInfo.Uuid);

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
                    if (!texture) texture = TextureCache::GetDefaultNormal();
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
            }

            texture->Bind(slot++);
        }
    }

    void ForwardTechnique::Resize(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;
        m_MainFBO->Resize(width, height);
    }
}
