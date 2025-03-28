#include "luthpch.h"
#include "luth/core/App.h"

#include "luth/window/Window.h"
#include "luth/input/Input.h"
#include "luth/editor/Editor.h"
#include "luth/resources/ResourceManager.h"

#include "luth/events/Event.h"
#include "luth/events/AppEvent.h"
#include "luth/events/KeyEvent.h"

namespace Luth
{
    App::App(int argc, char** argv)
    {
        // TODO Luth + version - OS - renderAPI
        WindowSpec ws = ParseCommandLineArgs(argc, argv);
        ws.VSync = false;

        m_Window = Window::Create(ws);
        Input::SetWindow(m_Window->GetNativeWindow());
        Renderer::Init(ws.rendererAPI, m_Window->GetNativeWindow());
        ResourceManager::Init();
        Editor::Init(m_Window->GetNativeWindow()); 
    }

    App::~App() {}

    void App::Run()
    {
        OnInit();

        while (m_Running)
        {
            Time::Update();

            m_Window->OnUpdate();

            OnUpdate();

            if (!m_Window->IsMinimized())
                Renderer::DrawFrame();

            // Render UI (not yet implemented in vulkan)
            if (Renderer::GetAPI() == RendererAPI::API::OpenGL)
            {
                Editor::BeginFrame();
                Editor::Render();
                OnUIRender();
                Editor::EndFrame();
            }

            m_Window->SwapBuffers();
            Renderer::Clear();
        }

        OnShutdown();
    }

    void App::Close()
    {
        m_Running = false;
    }

    WindowSpec App::ParseCommandLineArgs(int argc, char** argv)
    {
        WindowSpec spec;
        spec.rendererAPI = RendererAPI::API::OpenGL;
        
        if (argc < 2) { // No arguments
            LH_CORE_WARN("Usage: {} [--vulkan|--rt]", argv[0]);
            LH_CORE_WARN("Initializing default [--opengl]");
            return spec;
        }

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--opengl") {
                spec.rendererAPI = RendererAPI::API::OpenGL;
                break;
            }
            else if (arg == "--vulkan") {
                spec.rendererAPI = RendererAPI::API::Vulkan;
                break;
            }
            else {  // Invalid argument
                LH_CORE_WARN("Unknown argument: {}", arg);
            }
        }
        return spec;
    }
}
