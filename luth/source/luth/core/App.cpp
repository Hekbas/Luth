#include "luthpch.h"
#include "luth/core/App.h"

#include "luth/window/Window.h"
#include "luth/input/Input.h"
#include "luth/events/Event.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/Resources.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/ECS/Systems.h"

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
        Resources::Init();
        ResourceDB::Init(FileSystem::AssetsPath());
        Systems::Init();
        Editor::Init(m_Window->GetNativeWindow());

        // Subscribe to events
        m_MainThreadEventBus->Subscribe<WindowResizeEvent>([this](Event& e) {
            OnWindowResize(static_cast<WindowResizeEvent&>(e));
        });

        m_MainThreadEventBus->Subscribe<WindowCloseEvent>([this](Event& e) {
            OnWindowClose(static_cast<WindowCloseEvent&>(e));
        });

        m_MainThreadEventBus->Subscribe<FileDropEvent>([this](Event& e) {
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
                Systems::Update<RenderingSystem>();

                // Render UI (not yet implemented in vulkan)
                if (Renderer::GetAPI() == RendererAPI::API::OpenGL)
                {
                    Editor::BeginFrame();
                    Editor::Render();
                    OnUIRender();
                    Editor::EndFrame();
                }
            }

            m_Window->SwapBuffers();
            Renderer::Clear();
        }

        OnShutdown();
        Close();
    }

    void App::Close()
    {
        ResourceDB::SaveDirty();
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
        for (const auto& srcPath : e.GetPaths()) {
            try {
                // 1. Validate file
                if (!fs::exists(srcPath)) {
                    LH_CORE_ERROR("Dropped file not found: {0}", srcPath.string());
                    continue;
                }

                // 2. Classify resource type
                ResourceType resType = FileSystem::ClassifyFileType(srcPath);
                if (resType == ResourceType::Unknown) {
                    LH_CORE_WARN("Unsupported file type: {0}", srcPath.string());
                    continue;
                }

                // 3. Determine destination path
                fs::path destPath = FileSystem::GetPath(resType, srcPath.stem().string(), true);

                // Create target directory if needed
                FileSystem::CreateDirectories(destPath.parent_path());

                // 4. Copy file to project
                fs::copy_file(srcPath, destPath, fs::copy_options::overwrite_existing);
                LH_CORE_INFO("Imported {0} to {1}", srcPath.filename().string(), destPath.string());

                // 5. Generate meta file
                UUID newUuid = MetaFile::Create(destPath, resType);

                LH_CORE_INFO("Created asset {0} with UUID {1}", destPath.filename().string(), newUuid.ToString());
            }
            catch (const fs::filesystem_error& err) {
                LH_CORE_ERROR("Import failed: {0} - {1}", srcPath.string(), err.what());
            }
            catch (const std::exception& ex) {
                LH_CORE_ERROR("Asset processing error: {0} - {1}", srcPath.string(), ex.what());
            }
        }
    }
}
