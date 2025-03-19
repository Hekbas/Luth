#include <Luth.h>
#include "OpenGLApp.h"

#include <imgui.h>

// TEST
#include <luth/resources/ShaderLibrary.h>
#include <luth/resources/ResourceManager.h>
#include <luth/resources/Loaders.h>

#include <luth/renderer/Renderer.h>
#include <luth/renderer/Buffer.h>
#include <luth/renderer/Shader.h>
#include <luth/renderer/Texture.h>
#include <luth/renderer/Model.h>

#include <luth/renderer/openGL/GLRendererAPI.h>
#include <luth/renderer/openGL/GLBuffer.h>
#include <luth/renderer/openGL/GLMesh.h>

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
        LoadShader();

        Model model(ResourceManager::GetPath(Resource::Model, "grimoire/MimicBook.fbx"));
        std::vector<std::shared_ptr<GLMesh>> meshes;

        for (const auto& meshData : model.GetMeshes()) {
            const auto& material = model.GetMaterials()[meshData.MaterialIndex];

            // Load texture
            auto texture = material.Textures.empty()
                ? std::make_shared<GLTexture>(ResourceManager::GetPath(Resource::Texture, "container.jpg"))
                : std::make_shared<GLTexture>(material.Textures[0].path);

            // Create buffers
            auto vb = std::make_shared<GLVertexBuffer>(meshData.Vertices.data(),
                meshData.Vertices.size() * sizeof(Vertex));
            vb->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                            { ShaderDataType::Float3, "a_Normal"   },
                            { ShaderDataType::Float2, "a_TexCoord" } }
            );

            auto ib = std::make_shared<GLIndexBuffer>(meshData.Indices.data(),
                meshData.Indices.size());

            meshes.push_back(std::make_shared<GLMesh>(vb, ib, texture));
        }

        Renderer::SubmitMesh(meshes[0]);
    }

    void OpenGLApp::OnUpdate()
    {
        Mat4 model = glm::rotate(
            Mat4(1.0f),
            Time::GetTime() * glm::radians(45.0f),
            Vec3(0.0f, 0.0f, 1.0f)
        );
        Mat4 view = glm::lookAt(
            Vec3(2.5f, 2.5f, 2.5f),
            Vec3(0.0f, 0.0f, 0.0f),
            Vec3(0.0f, 0.0f, 1.0f)
        );
        Mat4 proj = glm::perspective(
            glm::radians(45.0f),
            16.0f / 9.0f,
            0.1f, 10.0f
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
        shader->SetInt("u_Texture", 0);
    }
}
