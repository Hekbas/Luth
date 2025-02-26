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

namespace Luth
{
    App::App()
    {
        // TODO Luth + version - OS - renderAPI
        WindowSpec ws;
        ws.rendererAPI = RendererAPI::API::OpenGL;

        m_Window = Window::Create(ws);
        Input::SetWindow(m_Window->GetNativeWindow());
        Renderer::Init(ws.rendererAPI, m_Window->GetNativeWindow());
        Editor::Init(m_Window->GetNativeWindow());
    }

    App::~App() {}

    void App::Run()
    {
        OnInit();

        while (m_Running)
        {
            // Calculate timestep
            f32 time = Timestep::GetTime();
            f32 dt = time - m_LastFrameTime;
            m_LastFrameTime = time;

            // Update window first
            m_Window->OnUpdate();

            // User-defined update
            OnUpdate(dt);

            // Render UI
            Editor::BeginFrame();
            OnUIRender();
            Editor::EndFrame();

            m_Window->SwapBuffers();
            Renderer::Clear();
        }

        OnShutdown();
    }

    void App::Close()
    {
        m_Running = false;
    }
}
