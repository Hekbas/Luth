#include "Luthpch.h"
#include "luth/renderer/techniques/DeferredTechnique.h"
#include "luth/renderer/Renderer.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/ResourceDB.h"
#include "luth/resources/libraries/ShaderLibrary.h"
#include "luth/resources/libraries/MaterialLibrary.h"
#include "luth/resources/libraries/ModelLibrary.h"

namespace Luth
{
    void DeferredTechnique::Init(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;

        CreateGBuffer();
        CreateLightBuffer();

        // Load shaders
        UUID geo = ResourceDB::PathToUuid(FileSystem::GetPath(ResourceType::Shader, "LuthDeferredGeo"));
        UUID light = ResourceDB::PathToUuid(FileSystem::GetPath(ResourceType::Shader, "LuthDeferredLight"));
        m_GeometryShader = ShaderLibrary::Get(geo);
        m_LightingShader = ShaderLibrary::Get(light);
    }

    void DeferredTechnique::Shutdown()
    {
        m_GBuffer.reset();
        m_LightBuffer.reset();
    }

    void DeferredTechnique::Render(entt::registry& registry,
        const glm::vec3& cameraPos,
        const std::vector<RenderCommand>& opaque,
        const std::vector<RenderCommand>& transparent)
    {
        // Geometry Pass: Fill G-Buffer
        GeometryPass(opaque);

        // Lighting Pass: Compute illumination
        LightingPass(registry);

        // Forward pass for transparent objects
        Renderer::EnableBlending(true);
        //Renderer::SetDepthWrite(false);
        for (const auto& cmd : transparent) {
            // Implement transparent forward rendering if needed
        }
        //Renderer::SetDepthWrite(true);
        Renderer::EnableBlending(false);
    }

    void DeferredTechnique::Resize(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;
        m_GBuffer->Resize(width, height);
        m_LightBuffer->Resize(width, height);
    }

    void DeferredTechnique::SetViewProjection(const glm::mat4& vp)
    {
        m_ViewProjection = vp;
    }

    u32 DeferredTechnique::GetFinalColorAttachment() const
    {
        return m_LightBuffer->GetColorAttachmentID();
    }

    std::vector<std::pair<std::string, u32>> DeferredTechnique::GetAllAttachments() const
    {
        // TODO: FIXME I'm only returning one fb!!!
        return m_GBuffer->GetAllAttachments();
    }

    // Private implementation
    void DeferredTechnique::GeometryPass(const std::vector<RenderCommand>& commands)
    {
        m_GBuffer->Bind();
        Renderer::Clear(BufferBit::Color | BufferBit::Depth);

        m_GeometryShader->Bind();
        m_GeometryShader->SetMat4("u_ViewProjection", m_ViewProjection);

        for (const auto& cmd : commands) {
            auto& transform = *cmd.transform;
            auto& meshRend = *cmd.meshRend;

            auto model = ModelLibrary::Get(meshRend.ModelUUID);
            auto material = MaterialLibrary::Get(meshRend.MaterialUUID);
            if (!model || !material) continue;

            // Transform and material properties
            m_GeometryShader->SetMat4("u_Model", transform.GetTransform());

            // Bind material textures
            SetupMaterialTextures(*material, *m_GeometryShader);

            model->GetMeshes()[meshRend.MeshIndex]->Draw();
        }

        m_GBuffer->Unbind();
    }

    void DeferredTechnique::LightingPass(entt::registry& registry)
    {
        m_LightBuffer->Bind();
        Renderer::Clear(BufferBit::Color);

        m_LightingShader->Bind();

        // Bind G-Buffer textures
        m_GBuffer->BindColorAsTexture(GBufferColorIndex::Position,  0);
        m_GBuffer->BindColorAsTexture(GBufferColorIndex::Normal,    1);
        m_GBuffer->BindColorAsTexture(GBufferColorIndex::Albedo,    2);
        m_GBuffer->BindColorAsTexture(GBufferColorIndex::MRAO,      3);

        Renderer::DrawFullscreenQuad();

        m_LightBuffer->Unbind();
    }

    void DeferredTechnique::CreateGBuffer()
    {
        Framebuffer::Spec spec{
            .Width = m_Width,
            .Height = m_Height,
            .ColorAttachments = {
                {.InternalFormat = GL_RGBA16F},     // Position + Depth
                {.InternalFormat = GL_RGB16F},      // Normal
                {.InternalFormat = GL_RGBA8},       // Albedo
                {.InternalFormat = GL_RGB8}         // Metallic/Roughness/AO
            },
            .DepthStencilAttachment = {
                {.InternalFormat = GL_DEPTH24_STENCIL8, .IsTexture = false}
            }
        };
        m_GBuffer = Framebuffer::Create(spec);
    }

    void DeferredTechnique::CreateLightBuffer()
    {
        Framebuffer::Spec spec{
            .Width = m_Width,
            .Height = m_Height,
            .ColorAttachments = {{.InternalFormat = GL_RGBA16F}}    // Final lit color
        };
        m_LightBuffer = Framebuffer::Create(spec);
    }

    void DeferredTechnique::SetupMaterialTextures(const Material& material, Shader& shader)
    {
        int slot = 0;
        for (const auto& texInfo : material.GetTextures()) {
            auto texture = TextureCache::Get(texInfo.TextureUuid);

            switch (texInfo.type) {
                case MapType::Diffuse:
                    if (!texture) texture = TextureCache::GetDefaultWhite();
                    shader.SetInt("u_TexDiffuse", slot);
                    shader.SetInt("u_UVIndexDiffuse", texInfo.uvIndex);
                    break;
                case MapType::Alpha:
                    if (!texture) texture = TextureCache::GetDefaultWhite();
                    shader.SetInt("u_TexAlpha", slot);
                    shader.SetInt("u_UVIndexAlpha", texInfo.uvIndex);
                    break;
                case MapType::Normal:
                    if (!texture) texture = TextureCache::GetDefaultNormal();
                    shader.SetInt("u_TexNormal", slot);
                    shader.SetInt("u_UVIndexNormal", texInfo.uvIndex);
                    break;
                case MapType::Emissive:
                    if (!texture) texture = TextureCache::GetDefaultBlack();
                    shader.SetInt("u_TexEmissive", slot);
                    shader.SetInt("u_UVIndexEmissive", texInfo.uvIndex);
                    break;
                case MapType::Metalness:
                    if (!texture) texture = TextureCache::GetDefaultGrey();
                    shader.SetInt("u_TexMetallic", slot);
                    shader.SetInt("u_UVIndexMetallic", texInfo.uvIndex);
                    break;
                case MapType::Roughness:
                    if (!texture) texture = TextureCache::GetDefaultGrey();
                    shader.SetInt("u_TexRoughness", slot);
                    shader.SetInt("u_UVIndexRoughness", texInfo.uvIndex);
                    break;
                case MapType::Specular:
                    if (!texture) texture = TextureCache::GetDefaultGrey();
                    shader.SetInt("u_TexSpecular", slot);
                    shader.SetInt("u_UVIndexSpecular", texInfo.uvIndex);
                    break;
                case MapType::Oclusion:
                    if (!texture) texture = TextureCache::GetDefaultWhite();
                    shader.SetInt("u_TexOclusion", slot);
                    shader.SetInt("u_UVIndexOclusion", texInfo.uvIndex);
                    break;
            }

            texture->Bind(slot++);
        }

        // Set material properties
        shader.SetVec4("u_Color", material.GetColor());
        shader.SetFloat("u_Alpha", material.GetAlpha());
        //shader.SetFloat("u_Metallic", material.GetMetallic());
        //shader.SetFloat("u_Roughness", material.GetRoughness());
        //shader.SetFloat("u_AO", material.GetAO());
    }
}
