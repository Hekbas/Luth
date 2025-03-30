#include "luthpch.h"
#include "luth/core/App.h"

#include "luth/window/Window.h"
#include "luth/input/Input.h"
#include "luth/events/Event.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/Resources.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"

namespace Luth
{
    App::App(int argc, char** argv) : m_MainThreadEventBus(std::make_unique<EventBus>())
    {
        // TODO Luth + version - OS - renderAPI
        WindowSpec ws = ParseCommandLineArgs(argc, argv);
        ws.VSync = false;
        ws.EventBus = m_MainThreadEventBus;

        m_Window = Window::Create(ws);
        Input::SetWindow(m_Window->GetNativeWindow());
        Renderer::Init(ws.rendererAPI, m_Window->GetNativeWindow());
        FileSystem::Init();
        Resources::InitLibraries();
        Editor::Init(m_Window->GetNativeWindow());

        // Subscribe to events
        m_MainThreadEventBus->Subscribe<WindowResizeEvent>([this](Luth::Event& e) {
            OnWindowResize(static_cast<WindowResizeEvent&>(e));
        });

        m_MainThreadEventBus->Subscribe<WindowCloseEvent>([this](Luth::Event& e) {
            OnWindowClose(static_cast<WindowCloseEvent&>(e));
        });

        m_MainThreadEventBus->Subscribe<FileDropEvent>([this](Luth::Event& e) {
            OnFileDrop(static_cast<FileDropEvent&>(e));
        });
    }

    App::~App() {}

    void App::Run()
    {
        OnInit();

        while (m_Running)
        {
            Time::Update();
            m_Window->OnUpdate();
            m_MainThreadEventBus->ProcessEvents();

            OnUpdate();

            if (!m_Window->IsMinimized())
            {
                auto sceneFb = Editor::GetPanel<ScenePanel>()->GetFramebuffer();
                Renderer::BindFramebuffer(sceneFb);
                Renderer::Clear();
                Renderer::DrawFrame();
                Renderer::BindFramebuffer(nullptr);
            }

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

    void App::OnWindowResize(WindowResizeEvent& e)
    {

    }

    void App::OnWindowClose(WindowCloseEvent& e)
    {
        m_Running = false;
    }

    void App::OnFileDrop(FileDropEvent& e)
    {
        //Resources::ImportAsset(e.GetPaths());
    }
}
