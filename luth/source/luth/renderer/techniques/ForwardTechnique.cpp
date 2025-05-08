#include "Luthpch.h"
#include "luth/renderer/techniques/ForwardTechnique.h"
#include "luth/renderer/Renderer.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/ResourceDB.h"
#include "luth/resources/libraries/MaterialLibrary.h"
#include "luth/resources/libraries/ModelLibrary.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"

namespace Luth
{
    ForwardTechnique::ForwardTechnique() : RenderTechnique("Forward") { Init(1280, 720); }

    void ForwardTechnique::Init(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;

        UUID geo        = ResourceDB::PathToUuid(FileSystem::GetPath(ResourceType::Shader, "LuthGeo"));
        UUID ssao       = ResourceDB::PathToUuid(FileSystem::GetPath(ResourceType::Shader, "LuthSSAO"));
        UUID ssaoBlur   = ResourceDB::PathToUuid(FileSystem::GetPath(ResourceType::Shader, "LuthSSAOBlur"));
        UUID bloom      = ResourceDB::PathToUuid(FileSystem::GetPath(ResourceType::Shader, "LuthBloomExtract"));
        UUID bloomBlur  = ResourceDB::PathToUuid(FileSystem::GetPath(ResourceType::Shader, "LuthBloomBlur"));
        UUID composite  = ResourceDB::PathToUuid(FileSystem::GetPath(ResourceType::Shader, "LuthComposite"));
        m_GeoShader         = ShaderLibrary::Get(geo);
        m_SSAOShader        = ShaderLibrary::Get(ssao);
        m_SSAOBlurShader    = ShaderLibrary::Get(ssaoBlur);
        m_BloomExtShader    = ShaderLibrary::Get(bloom);
        m_BloomBlurShader   = ShaderLibrary::Get(bloomBlur);
        m_CompositeShader   = ShaderLibrary::Get(composite);

        // Main forward buffer
        m_MainFBO = Framebuffer::Create({
            .Width = width,
            .Height = height,
            .ColorAttachments = {{.InternalFormat = GL_RGBA16F}},
            .DepthStencilAttachment = {{.InternalFormat = GL_DEPTH24_STENCIL8}}
        });

        // Geometry buffer for SSAO
        m_GeometryFBO = Framebuffer::Create({
            .Width = width,
            .Height = height,
            .ColorAttachments = {
                {.InternalFormat = GL_RGB16F, .Name = "Position"},
                {.InternalFormat = GL_RGB16F, .Name = "Normal"}
            }
        });

        // SSAO buffers
        m_SSAOFBO = Framebuffer::Create({
            .Width = width,
            .Height = height,
            .ColorAttachments = {{.InternalFormat = GL_R8}}
        });

        m_SSAOBlurFBO = Framebuffer::Create({
            .Width = width,
            .Height = height,
            .ColorAttachments = {{.InternalFormat = GL_R8}}
        });

        // Bloom buffers
        Framebuffer::Spec bloomSpec{
            .Width = width / 2,
            .Height = height / 2,
            .ColorAttachments = {{.InternalFormat = GL_RGBA16F}}
        };
        m_BrightnessFBO = Framebuffer::Create(bloomSpec);
        m_PingPongFBO[0] = Framebuffer::Create(bloomSpec);
        m_PingPongFBO[1] = Framebuffer::Create(bloomSpec);

        InitSSAOKernel();
        InitNoiseTexture();
    }

    void ForwardTechnique::Shutdown()
    {
        m_SSAOKernel.clear();
    }

    void ForwardTechnique::Render(entt::registry& registry,
        const Vec3& cameraPos,
        const std::vector<RenderCommand>& opaque,
        const std::vector<RenderCommand>& transparent)
    {
        // Geometry Pre-Pass
        m_GeometryFBO->Bind();
        Renderer::Clear(BufferBit::Color);
        RenderGeometryPrepass(opaque);
        m_GeometryFBO->Unbind();

        // Main Forward Pass
        m_MainFBO->Bind();
        Renderer::Clear(BufferBit::Color | BufferBit::Depth);
        RenderForwardPass(opaque, cameraPos, true);
        RenderForwardPass(transparent, cameraPos, false);
        m_MainFBO->Unbind();

        // Post-Processing
        RenderSSAOPass();
        RenderBloomPass();
        RenderCompositePass();
    }

    void ForwardTechnique::Resize(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;

        m_MainFBO->Resize(width, height);
        m_GeometryFBO->Resize(width, height);
        m_SSAOFBO->Resize(width, height);
        m_SSAOBlurFBO->Resize(width, height);

        // Bloom buffers stay at half resolution
        u32 bloomWidth = width / 2, bloomHeight = height / 2;
        m_BrightnessFBO->Resize(bloomWidth, bloomHeight);
        m_PingPongFBO[0]->Resize(bloomWidth, bloomHeight);
        m_PingPongFBO[1]->Resize(bloomWidth, bloomHeight);
    }

    u32 ForwardTechnique::GetFinalColorAttachment() const
    {
        return m_MainFBO->GetColorAttachmentID(0);
    }

    std::vector<std::pair<std::string, u32>> ForwardTechnique::GetAllAttachments() const
    {
        return {
            { "Main Color", m_MainFBO->GetColorAttachmentID(0)        },
            { "Position",   m_GeometryFBO->GetColorAttachmentID(0)    },
            { "Normal",     m_GeometryFBO->GetColorAttachmentID(1)    },
            { "SSAO Raw",   m_SSAOFBO->GetColorAttachmentID(0)        },
            { "SSAO Blur",  m_SSAOBlurFBO->GetColorAttachmentID(0)    },
            { "Bloom",      m_PingPongFBO[0]->GetColorAttachmentID(0) }
        };
    }

    void ForwardTechnique::RenderGeometryPrepass(const std::vector<RenderCommand>& commands)
    {
        m_GeoShader->Bind();

        for (const auto& cmd : commands) {
            RenderMesh(cmd, true);
        }
    }

    void ForwardTechnique::RenderForwardPass(const std::vector<RenderCommand>& commands,
        const Vec3& cameraPos, bool isOpaque)
    {
        m_SSAOBlurFBO->BindColorAsTexture(0, 5); // SSAO

        for (const auto& cmd : commands) {
            RenderMesh(cmd, isOpaque);
        }
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
                case MapType::Thickness:    texture = TextureCache::GetDefaultBlack();  break;
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
        shader->SetVec3("u_Subsurface.color", material->GetSubsurface().color);
        shader->SetFloat("u_Subsurface.strength", material->GetSubsurface().strength);
        shader->SetFloat("u_Subsurface.thicknessScale", material->GetSubsurface().thicknessScale);
    }

    void ForwardTechnique::RenderSSAOPass()
    {
        // Calculate SSAO
        m_SSAOFBO->Bind();
        m_SSAOShader->Bind();

        m_GeometryFBO->BindColorAsTexture(0, 0); // Position
        m_GeometryFBO->BindColorAsTexture(1, 1); // Normal
        //m_NoiseTexture->Bind(2);

        //m_SSAOShader->SetMat4("u_Projection", camera.GetProjectionMatrix());
        //m_SSAOShader->SetUniform("u_Samples", m_SSAOKernel.data(), m_SSAOKernel.size());
        //m_SSAOShader->SetFloat("u_Radius", m_SSAORadius);
        Renderer::DrawFullscreenQuad();
        m_SSAOFBO->Unbind();

        // Blur SSAO
        m_SSAOBlurFBO->Bind();
        m_SSAOBlurShader->Bind();
        m_SSAOFBO->BindColorAsTexture(0, 0);
        Renderer::DrawFullscreenQuad();
        m_SSAOBlurFBO->Unbind();
    }

    void ForwardTechnique::RenderBloomPass()
    {
        // Brightness extraction
        {
            m_BrightnessFBO->Bind();
            m_BloomExtShader->Bind();

            // Bind main color texture at full resolution
            m_MainFBO->BindColorAsTexture(0, 0);
            m_BloomExtShader->SetFloat("u_Threshold", m_BloomThreshold);

            Renderer::DrawFullscreenQuad();
            m_BrightnessFBO->Unbind();
        }

        // Gaussian blur passes (ping-pong between two buffers)
        bool horizontal = true;
        bool firstIteration = true;
        const float blurStrength = 1.0f; // Adjust based on resolution

        for (int i = 0; i < m_BloomBlurPasses * 2; i++)
        {
            m_PingPongFBO[horizontal]->Bind();
            m_BloomBlurShader->Bind();

            // Bind input texture
            if (firstIteration) {
                m_BrightnessFBO->BindColorAsTexture(0, 0);
                firstIteration = false;
            }
            else {
                m_PingPongFBO[!horizontal]->BindColorAsTexture(0, 0);
            }

            // Set blur parameters
            m_BloomBlurShader->SetFloat("u_Horizontal", horizontal);
            m_BloomBlurShader->SetFloat("u_BlurStrength", blurStrength);

            Renderer::DrawFullscreenQuad();
            m_PingPongFBO[horizontal]->Unbind();

            horizontal = !horizontal;
        }
    }

    void ForwardTechnique::RenderCompositePass()
    {
        Framebuffer::Unbind();
        m_CompositeShader->Bind();

        m_MainFBO->BindColorAsTexture(0, 0);        // Main color
        m_SSAOBlurFBO->BindColorAsTexture(0, 1);    // SSAO
        m_PingPongFBO[0]->BindColorAsTexture(0, 2); // Bloom

        m_CompositeShader->SetFloat("u_Exposure", 1.0f);
        m_CompositeShader->SetFloat("u_BloomStrength", 0.04f);
        Renderer::DrawFullscreenQuad();
    }

    void ForwardTechnique::InitSSAOKernel()
    {
        std::uniform_real_distribution<GLfloat> randomFloats(0.0, 1.0);
        std::default_random_engine generator;

        for (unsigned int i = 0; i < 64; ++i) {
            Vec3 sample(
                randomFloats(generator) * 2.0 - 1.0,
                randomFloats(generator) * 2.0 - 1.0,
                randomFloats(generator)
            );
            sample = glm::normalize(sample);
            sample *= randomFloats(generator);
            m_SSAOKernel.push_back(sample);
        }
    }

    void ForwardTechnique::InitNoiseTexture()
    {
        std::vector<Vec3> noise;
        std::uniform_real_distribution<GLfloat> randomFloats(0.0, 1.0);
        std::default_random_engine generator;

        for (unsigned int i = 0; i < 16; i++) {
            noise.push_back(Vec3(
                randomFloats(generator) * 2.0 - 1.0,
                randomFloats(generator) * 2.0 - 1.0,
                0.0f
            ));
        }

        //m_NoiseTexture = Texture2D::Create();
        //m_NoiseTexture->SetData(4, 4, noise.data());
        //m_NoiseTexture->SetFilter(GL_NEAREST);
    }
}
