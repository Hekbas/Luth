#include "luthpch.h"
#include "luth/core/App.h"
#include "luth/core/Log.h"
#include "luth/core/Window.h"
#include "luth/events/Event.h"
#include "luth/events/AppEvent.h"
#include "luth/events/KeyEvent.h"

namespace Luth
{
    App::App()
    {
        // Initialize window with event bus reference
        WindowSpec spec;
        spec.Title = "Luth Engine";
        m_Window = std::make_unique<Window>(m_EventBus, spec);

        // Subscribe to core events
        m_EventBus.Subscribe<WindowCloseEvent>([this](Event& e) { Close(); });
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
            f32 dt = time - m_LastFrameTime;
            m_LastFrameTime = time;

            // Update window first
            m_Window->OnUpdate();

            // Process all events
            ProcessEvents();

            // User-defined update
            OnUpdate(dt);
        }

        OnShutdown();
    }

    void App::Close()
    {
        m_Running = false;
    }

    void App::ProcessEvents()
    {
        m_EventBus.ProcessEvents();
    }

    void App::OnEvent(Event& event)
    {
    }
}
