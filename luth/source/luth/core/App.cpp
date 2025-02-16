#include "luthpch.h"
#include "luth/core/App.h"
#include "luth/core/Log.h"
#include "luth/core/Layer.h"
#include "luth/core/Window.h"

namespace Luth
{
    App::App()
    {
        // Initialize window with event bus reference
        WindowSpec spec;
        spec.Title = "Luth Engine";
        m_Window = std::make_unique<Window>(spec);
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
            f32 deltaTime = time - m_LastFrameTime;
            m_LastFrameTime = time;

            // Update window first
            m_Window->OnUpdate();

            // Process all events
            ProcessEvents();

            // User-defined update
            OnUpdate(deltaTime);
        }

        OnShutdown();
    }

    void App::ProcessEvents()
    {
        m_EventBus.ProcessEvents();
    }

    void App::Close()
    {
        m_Running = false;
    }

    void App::OnEvent(Event& event)
    {
    }
}
