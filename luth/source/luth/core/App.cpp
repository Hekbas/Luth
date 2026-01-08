#include "luthpch.h"
#include "luth/core/App.h"

#include "luth/window/Window.h"
#include "luth/input/Input.h"
#include "luth/events/Event.h"
#include "luth/resources/FileSystem.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/ECS/Systems.h"
#include "luth/ECS/systems/TransformSystem.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Profiler.h"

namespace Luth
{
    App::App(int argc, char** argv)
    {
        // Core Systems Init
        JobSystem::Init();
        FileSystem::Init();

        WindowSpec ws = ParseCommandLineArgs(argc, argv);
        SetAppTitle(ws);
        m_Window = Window::Create(ws);
        Input::SetWindow(m_Window->GetNativeWindow());

        // Renderer Init will happen here in Phase 3 (VulkanContext::Init)
        
        // Scene & Systems
        m_Scene = std::make_shared<Scene>();
        Systems::Init();
        Systems::SetRegistry(m_Scene->RegistryPtr());

        //Editor::Init(m_Window.get());

        // Subscribe to events
        EventBus::Subscribe<WindowResizeEvent>(BusType::MainThread, [this](Event& e) {
            OnWindowResize(static_cast<WindowResizeEvent&>(e));
        });

        EventBus::Subscribe<WindowCloseEvent>(BusType::MainThread, [this](Event& e) {
            OnWindowClose(static_cast<WindowCloseEvent&>(e));
        });

        EventBus::Subscribe<FileDropEvent>(BusType::MainThread, [this](Event& e) {
            OnFileDrop(static_cast<FileDropEvent&>(e));
        });
    }

    App::~App() {}

    void App::Run()
    {
        OnInit();

        while (m_Running)
        {
            LH_PROFILE_FRAME("MainThread");

            Time::Update();
            m_Window->OnUpdate();
            EventBus::ProcessEvents(BusType::MainThread);
            
            OnUpdate();

            if (!m_Window->IsMinimized())
            {
                {
                    LH_PROFILE_SCOPE("Systems::Update");
                    Systems::Update<TransformSystem>();
                    // RenderingSystem will be re-added in Phase 6
                }
            }

            // SwapBuffers / EndFrame will be handled by the new Renderer in Phase 3
        }

        OnShutdown();
        Close();
    }

    void App::Close()
    {
		Editor::Shutdown();
		Systems::Shutdown();
        
        // Renderer::Shutdown(); // Phase 3

        JobSystem::Shutdown();
    }

    WindowSpec App::ParseCommandLineArgs(int argc, char** argv)
    {
        WindowSpec spec;
        // Arguments parsing can be expanded later if needed
        return spec;
    }

    void App::SetAppTitle(WindowSpec& ws)
    {
		std::string title = "Luth 0.1";
        title += " [Vulkan]";

#ifdef _WIN32
		title += " - Windows";
#endif

		ws.Title = title;
    }

    void App::OnWindowResize(WindowResizeEvent& e)
    {
        // Renderer::Resize(e.GetWidth(), e.GetHeight()); // Phase 3
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
