#include "luthpch.h"
#include "luth/core/App.h"
#include "luth/core/Log.h"
#include "luth/core/Timestep.h"
#include "luth/window/Window.h"
#include "luth/input/Input.h"
#include "luth/editor/Editor.h"
#include "luth/events/Event.h"
#include "luth/events/AppEvent.h"
#include "luth/events/KeyEvent.h"
#include "luth/resources/ShaderLibrary.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/Shader.h"

// TEST
#include <glad/glad.h>    // OpenGL functions
#include <GLFW/glfw3.h>   // Window/context management
#include <glm/glm.hpp>    // GLM types
#include <memory>         // For smart pointers

namespace Luth
{
    App::App()
    {
        // TODO Luth + version - OS - renderAPI
        WindowSpec spec;
        spec.Title = "Luth Engine";

        m_Window = std::make_unique<Window>(spec);
        Input::SetWindow(m_Window->GetNativeWindow());
        Editor::Init(m_Window->GetNativeWindow());

        // TEST //////////////////////////////
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

    App::~App()
    {
    }

    void App::Run()
    {
        OnInit();

        while (m_Running)
        {
            // Calculate timestep
            f32 time = static_cast<f32>(glfwGetTime());
            //Timestep dt(time - m_LastFrameTime);
            f32 dt = time - m_LastFrameTime;
            m_LastFrameTime = time;

            // Update window first
            m_Window->OnUpdate();

            // User-defined update
            OnUpdate(dt);

            // TEST ZONE //////////////////////////////////////////////////////

            auto shader = Shader::Create("C:/Users/Hekbas/CITM/5_TFG/Luth/luthien/resources/mandelbrott.glsl");
            
            shader->Bind();
            shader->SetFloat("u_Time", time);
            //shader->SetVec2("u_Resolution",  glm::vec2((f32)m_Window->GetWidth(), (f32)m_Window->GetHeight()));
            glBindVertexArray(quadVAO);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            shader->Unbind();

            ///////////////////////////////////////////////////////////////////

            // Render UI
            Editor::BeginFrame();
            OnUIRender();
            Editor::EndFrame();

            m_Window->SwapBuffers();
        }

        OnShutdown();
    }

    void App::Close()
    {
        m_Running = false;
    }
}
