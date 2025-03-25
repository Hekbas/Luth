#include <Luth.h>
#include "OpenGLApp.h"

#include <imgui.h>

#include <luth/renderer/openGL/GLRendererAPI.h>
#include <luth/renderer/openGL/GLBuffer.h>
#include <luth/renderer/openGL/GLMesh.h>

#include <luth/resources/Resources.h>
#include <luth/resources/TextureCache.h>

#include <memory>

// TEST OPENGL
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

namespace Luth
{
    OpenGLApp::OpenGLApp(int argc, char** argv) : App(argc, argv) {}

    void OpenGLApp::OnInit()
    {
        InitUniformBuffer();
        //LoadShader();
        
        std::shared_ptr model = Resources::Load<Model>("mf/mf_f2.fbx");
        //std::shared_ptr model = Resources::Load<Model>("xeno/XenoRaven.fbx");

        std::shared_ptr shader = Resources::Load<Shader>("triangle.glsl");
        shader->Bind();
        shader->SetInt("u_TexDiffuse",   0);
        shader->SetInt("u_TexNormal",    1);
        shader->SetInt("u_TexEmissive",  2);
        shader->SetInt("u_TexMetallic",  3);
        shader->SetInt("u_TexRoughness", 4);
        shader->SetInt("u_TexSpecular",  5);

        for (auto& meshData : model->GetMeshes()) {

            // Create Material
            auto& material = model->GetMaterials()[meshData.materialIndex];

            material.SetShader(shader);
            std::vector<std::shared_ptr<Texture>> textures;

            for (auto texture : material.GetTextures()) {
                textures.push_back(Resources::Load<Texture>(texture.path.string()));
            }

            //MaterialLibrary::Add("", material);

            // Create buffers
            auto vb = std::make_shared<GLVertexBuffer>(meshData.vertices.data(),
                meshData.vertices.size() * sizeof(Vertex));
            vb->SetLayout({ { ShaderDataType::Float3, "a_Position"  },
                            { ShaderDataType::Float3, "a_Normal"    },
                            { ShaderDataType::Float2, "a_TexCoord0" },
                            { ShaderDataType::Float2, "a_TexCoord1" },
                            { ShaderDataType::Float3, "a_Tangent"   } }
            );

            auto ib = std::make_shared<GLIndexBuffer>(meshData.indices.data(),
                meshData.indices.size());

            Renderer::SubmitMesh(Mesh::Create(vb, ib, std::make_shared<Material>(material)));
        }
    }

    void OpenGLApp::OnUpdate()
    {
        Mat4 model = glm::rotate(
            Mat4(1.0f),
            Time::GetTime() * glm::radians(45.0f),
            Vec3(0.0f, 1.0f, 0.0f)
        );
        Mat4 view = glm::lookAt(
            Vec3(250.0f, 200.0f, 250.0f),
            Vec3(0.0f, 0.0f, 0.0f),
            Vec3(0.0f, 1.0f, 0.0f)
        );
        Mat4 proj = glm::perspective(
            glm::radians(45.0f),
            16.0f / 9.0f,
            0.1f, 10000.0f
        );

        UpdateUniforms(model, view, proj);
    }

    void OpenGLApp::OnUIRender()
    {
        // ImGui Demo
        static bool showDemo = true;
        if (showDemo) ImGui::ShowDemoWindow(&showDemo);

        ImGuiIO& io = ImGui::GetIO();
        ImGui::Begin("Luth Metrics");
        ImGui::Text("Frame time %.3f ms (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
        ImGui::End();
    }

    void OpenGLApp::OnShutdown()
    {
    }

    void OpenGLApp::InitUniformBuffer()
    {
        glGenBuffers(1, &m_UniformBuffer);
        glBindBuffer(GL_UNIFORM_BUFFER, m_UniformBuffer);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(Mat4) * 3, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_UniformBuffer);
    }

    void OpenGLApp::UpdateUniforms(const Mat4& model, const Mat4& view, const Mat4& proj)
    {
        glBindBuffer(GL_UNIFORM_BUFFER, m_UniformBuffer);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(Mat4), &model);
        glBufferSubData(GL_UNIFORM_BUFFER, sizeof(Mat4), sizeof(Mat4), &view);
        glBufferSubData(GL_UNIFORM_BUFFER, 2 * sizeof(Mat4), sizeof(Mat4), &proj);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    void OpenGLApp::LoadShader()
    {
        std::filesystem::path shaderPath = ResourceManager::GetPath(Resource::Shader, "triangle.glsl");
        shader = Shader::Create(shaderPath.generic_string());
        shader->Bind();
        shader->SetInt("u_TexDiffuse",   0);
        shader->SetInt("u_TexNormal",    1);
        shader->SetInt("u_TexMetallic",  2);
        shader->SetInt("u_TexRoughness", 3);
        shader->SetInt("u_TexSpecular",  4);
    }

    void OpenGLApp::InitScreenQuad()
    {
        float vertices[] =
        {
            // Positions   // TexCoords
            -1.0f,  1.0f,  0.0f, 1.0f,
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f, -1.0f,  1.0f, 0.0f,

            -1.0f,  1.0f,  0.0f, 1.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 1.0f
        };

        glGenVertexArrays(1, &quadVAO);
        glGenBuffers(1, &quadVBO);

        glBindVertexArray(quadVAO);
        glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

        // Position attribute
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

        // Texture coordinate attribute
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

        glBindVertexArray(0);
    }
}
