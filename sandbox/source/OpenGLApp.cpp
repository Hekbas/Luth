#include <Luth.h>
#include "OpenGLApp.h"

#include <imgui.h>

// TEST
#include <luth/resources/ShaderLibrary.h>
#include <luth/resources/ResourceManager.h>

#include <luth/renderer/Renderer.h>
#include <luth/renderer/Buffer.h>
#include <luth/renderer/Shader.h>
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
        //InitScreenQuad();
        InitUniformBuffer();
        LoadShader();

        const std::vector<GLVertex> vertices = {
            { {-0.5f, -0.5f}, {1.0f, 0.0f, 0.0f} },
            { { 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f} },
            { { 0.5f,  0.5f}, {0.0f, 0.0f, 1.0f} },
            { {-0.5f,  0.5f}, {1.0f, 1.0f, 1.0f} }
        };

        const std::vector<uint32_t> indices = {
            0, 1, 2, 2, 3, 0
        };

        BufferLayout layout = {
            { ShaderDataType::Float2, "inPosition" },
            { ShaderDataType::Float3, "inColor"    }
        };

        auto vkRenderer = static_cast<GLRendererAPI*>(Renderer::GetRendererAPI());
        auto vb = std::make_shared<GLVertexBuffer>(vertices.data(), sizeof(GLVertex) * vertices.size());
        vb->SetLayout(layout);
        auto ib = std::make_shared<GLIndexBuffer>(indices.data(), indices.size());
        auto mesh = std::make_shared<GLMesh>(vb, ib);

        Renderer::SubmitMesh(mesh);
    }

    void OpenGLApp::OnUpdate(f32 dt)
    {
        static float time = 0;
        time += dt;

        Mat4 model = glm::rotate(
            Mat4(1.0f),
            time * glm::radians(90.0f),
            Vec3(0.0f, 0.0f, 1.0f)
        );
        Mat4 view = glm::lookAt(
            Vec3(2.0f, 2.0f, 2.0f),
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
    }
}
