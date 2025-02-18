#include "luthpch.h"
#include "luth/core/App.h"
#include "luth/core/Log.h"
#include "luth/window/Window.h"
#include "luth/input/Input.h"
#include "luth/ui/UI.h"
#include "luth/events/Event.h"
#include "luth/events/AppEvent.h"
#include "luth/events/KeyEvent.h"

namespace Luth
{
    App::App()
    {
        // TODO Luth + version - OS - renderAPI
        WindowSpec spec;
        spec.Title = "Luth Engine";

        m_Window = std::make_unique<Window>(spec);
        Input::SetWindow(m_Window->GetNativeWindow());
        UI::Init(m_Window->GetNativeWindow());
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

            // User-defined update
            OnUpdate(dt);

            // Render UI
            UI::BeginFrame();
            // UI stuff here
            UI::EndFrame();
        }

        OnShutdown();
    }

    void App::Close()
    {
        m_Running = false;
    }
}
