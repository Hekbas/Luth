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
            },
            .DepthStencilAttachment = {{.InternalFormat = GL_DEPTH24_STENCIL8}}
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

        // Composite buffer
        m_CompositeFBO = Framebuffer::Create({
            .Width = width,
            .Height = height,
            .ColorAttachments = {{.InternalFormat = GL_RGBA16F}}
        });

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
        Renderer::Clear(BufferBit::Color | BufferBit::Depth);
        RenderGeometryPrepass(opaque);

        // Main Forward Pass
        m_MainFBO->Bind();
        Renderer::Clear(BufferBit::Color | BufferBit::Depth);
        RenderForwardPass(opaque, cameraPos);
        Renderer::EnableBlending(true);
		//Renderer::EnableDepthMask(false);
        RenderForwardPass(transparent, cameraPos);
        Renderer::EnableBlending(false);
        //Renderer::EnableDepthMask(true);

        // Post-Processing
        RenderSSAOPass();
        RenderBloomPass();
        RenderCompositePass();
    }

    void ForwardTechnique::Resize(u32 width, u32 height)
    {
        m_Width = width;
        m_Height = height;

        m_GeometryFBO->Resize(width, height);
        m_MainFBO->Resize(width, height);
        m_SSAOFBO->Resize(width, height);
        m_SSAOBlurFBO->Resize(width, height);
        m_CompositeFBO->Resize(width, height);

        // Bloom buffers stay at half resolution
        u32 bloomWidth = width / 2, bloomHeight = height / 2;
        m_BrightnessFBO->Resize(bloomWidth, bloomHeight);
        m_PingPongFBO[0]->Resize(bloomWidth, bloomHeight);
        m_PingPongFBO[1]->Resize(bloomWidth, bloomHeight);
    }

    u32 ForwardTechnique::GetFinalColorAttachment() const
    {
        return m_CompositeFBO->GetColorAttachmentID(0);
    }

    std::vector<std::pair<std::string, u32>> ForwardTechnique::GetAllAttachments() const
    {
        return {
            { "Main Color", m_MainFBO->GetColorAttachmentID(0)        },
            { "Position",   m_GeometryFBO->GetColorAttachmentID(0)    },
            { "Normal",     m_GeometryFBO->GetColorAttachmentID(1)    },
            { "SSAO Raw",   m_SSAOFBO->GetColorAttachmentID(0)        },
            { "SSAO Blur",  m_SSAOBlurFBO->GetColorAttachmentID(0)    },
			{ "Bloom Ext",  m_BrightnessFBO->GetColorAttachmentID(0)  },
			{ "Bloom Blur", m_PingPongFBO[0]->GetColorAttachmentID(0) }
        };
    }

    void ForwardTechnique::RenderGeometryPrepass(const std::vector<RenderCommand>& commands)
    {
        m_GeoShader->Bind();

        for (const auto& cmd : commands) {
            RenderMesh(cmd, *m_GeoShader);
        }
    }

    void ForwardTechnique::RenderForwardPass(const std::vector<RenderCommand>& commands, const Vec3& cameraPos)
    {
        //m_SSAOBlurFBO->BindColorAsTexture(0, 5); // SSAO

        for (const auto& cmd : commands) {
            // Fetch & bind material + shader
            auto& meshRend = *cmd.meshRend;
            auto  material = MaterialLibrary::Get(meshRend.MaterialUUID);
            if (!material) material = MaterialLibrary::Get(UUID(7));

            auto shader = material->GetShader();
            if (!shader) {
                LH_CORE_WARN("Invalid shader for material");
                continue;
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
            RenderMesh(cmd, *shader);
        }
    }

    void ForwardTechnique::RenderMesh(const RenderCommand& cmd, Shader& shader)
    {
        auto& meshRend = *cmd.meshRend;
        auto model = ModelLibrary::Get(meshRend.ModelUUID);
        if (!model) {
            LH_CORE_WARN("MeshRenderer missing model reference");
            return;
        }

        const auto& meshes = model->GetMeshes();
        if (meshRend.MeshIndex >= meshes.size()) {
            LH_CORE_ERROR("Invalid mesh index: {0}", meshRend.MeshIndex);
            return;
        }

        shader.SetMat4("u_Model", cmd.transform->GetTransform());

        meshes[meshRend.MeshIndex]->Draw();
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
        Renderer::Clear(BufferBit::Color);
        m_SSAOShader->Bind();

        m_GeometryFBO->BindColorAsTexture(0, 0);    // Position
        m_GeometryFBO->BindColorAsTexture(1, 1);    // Normal
        m_NoiseTexture->Bind(2);                    // Noise
        m_SSAOShader->SetInt("gPosition", 0);
        m_SSAOShader->SetInt("gNormal",   1);
        m_SSAOShader->SetInt("u_Noise",   2);

        m_SSAOShader->SetVec2("u_NoiseScale", { m_Width / 4, m_Height / 4 });
        m_SSAOShader->SetFloat("u_Radius", m_SSAORadius);
        m_SSAOShader->SetFloat("u_Bias", m_SSAOBias);
        Renderer::DrawFullscreenQuad();

        // Blur SSAO
        m_SSAOBlurFBO->Bind();
        Renderer::Clear(BufferBit::Color);
        m_SSAOBlurShader->Bind();
        m_SSAOFBO->BindColorAsTexture(0, 0);
        m_SSAOBlurShader->SetVec2("u_BlurScale", { 1.0f, 1.0f });
        Renderer::DrawFullscreenQuad();
        m_SSAOBlurFBO->Unbind();
    }

    void ForwardTechnique::RenderBloomPass()
    {
        // Brightness extraction
        {
            m_BrightnessFBO->Bind();
            Renderer::SetClearColor({ 0.0, 0.0, 0.0, 1.0 });
            Renderer::Clear(BufferBit::Color);
            Renderer::SetClearColor({ 0.15, 0.15, 0.15, 1.0 });
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
        const float blurStrength = 2.0f; // Adjust based on resolution

        for (int i = 0; i < m_BloomBlurPasses * 2; i++)
        {
            m_PingPongFBO[horizontal]->Bind();
            Renderer::Clear(BufferBit::Color);
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
            m_BloomBlurShader->SetFloat("u_BlurStrength", m_BloomStrength);

            Renderer::DrawFullscreenQuad();
            m_PingPongFBO[horizontal]->Unbind();

            horizontal = !horizontal;
        }
    }

    void ForwardTechnique::RenderCompositePass()
    {
		m_CompositeFBO->Bind();
        Renderer::Clear(BufferBit::Color);
        m_CompositeShader->Bind();

        m_MainFBO->BindColorAsTexture(0, 0);        // Main color
        m_SSAOBlurFBO->BindColorAsTexture(0, 1);    // SSAO
        m_PingPongFBO[0]->BindColorAsTexture(0, 2); // Bloom

        m_CompositeShader->SetInt("u_Scene", 0);
        m_CompositeShader->SetInt("u_SSAO",  1);
        m_CompositeShader->SetInt("u_Bloom", 2);

        m_CompositeShader->SetFloat("u_Exposure", m_Exposure);
        m_CompositeShader->SetFloat("u_BloomStrength", m_BloomStrength);
        m_CompositeShader->SetFloat("u_SSAOStrength", m_SSAOStrength);
        Renderer::DrawFullscreenQuad();
		m_CompositeFBO->Unbind();
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

        glGenBuffers(1, &m_SSBOKernel);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_SSBOKernel);
        glBufferData(GL_SHADER_STORAGE_BUFFER, m_SSAOKernel.size() * sizeof(Vec3), m_SSAOKernel.data(), GL_STATIC_READ);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_SSBOKernel);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    void ForwardTechnique::InitNoiseTexture()
    {
        constexpr int NOISE_SIZE = 4; // 4x4 texture
        std::vector<Vec3> noise;
        std::uniform_real_distribution<GLfloat> randomFloats(-1.0, 1.0);
        std::default_random_engine generator;

        // Generate 4x4 noise data (16 pixels)
        noise.reserve(NOISE_SIZE * NOISE_SIZE);
        for (int i = 0; i < NOISE_SIZE * NOISE_SIZE; i++) {
            noise.emplace_back(
                randomFloats(generator) * 2.0 - 1.0,
                randomFloats(generator) * 2.0 - 1.0,
                0.0f
            );
        }

        m_NoiseTexture = Luth::Texture::Create(
            NOISE_SIZE, NOISE_SIZE,
            TextureFormat::RGB8,
            reinterpret_cast<const float*>(noise.data())
        );

        m_NoiseTexture->SetWrapMode(TextureWrapMode::Repeat);
        m_NoiseTexture->SetFilterMode(TextureFilterMode::Nearest, TextureFilterMode::Nearest);
    }
}
