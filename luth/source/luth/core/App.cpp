#include "luthpch.h"
#include "luth/core/App.h"

#include "luth/window/Window.h"
#include "luth/input/Input.h"
#include "luth/events/Event.h"
#include "luth/events/AppEvent.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/MetaFile.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/ECS/Systems.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/ECS/systems/TransformSystem.h"
#include "luth/core/JobSystem.h"
#include "luth/core/Profiler.h"
#include "luth/renderer/Renderer.h"

namespace Luth
{
    App::App(int argc, char** argv)
    {
        // Core Systems Init
        JobSystem::Init();
        FileSystem::Init();
        AssetDatabase::Init(FileSystem::AssetsPath());
        AssetManager::Init();

        // Import Phase: Process any stale assets found by AssetDatabase
        const auto& dirtyAssets = AssetDatabase::GetDirtyAssets();
        if (!dirtyAssets.empty())
        {
            LH_CORE_INFO("Importing {0} assets in parallel...", dirtyAssets.size());
            
            JobSystem::Counter importCounter;
            // Stateless lambda converts to function pointer for JobSystem
            JobSystem::Dispatch((u32)dirtyAssets.size(), 1, [](JobSystem::JobArgs args) {
                const auto& assets = AssetDatabase::GetDirtyAssets();
                AssetManager::Import(assets[args.jobIndex]);
            }, nullptr, &importCounter);

            JobSystem::WaitForCounter(&importCounter);
        }

        WindowSpec ws = ParseCommandLineArgs(argc, argv);
        SetAppTitle(ws);
        m_Window = Window::Create(ws);
        Input::Init();

        Renderer::Init(m_Window->GetNativeWindow());
        
        // Scene & Systems
        m_Scene = std::make_shared<Scene>();
        Systems::Init();
        Systems::SetScene(m_Scene.get());

        Editor::Init(m_Window.get());

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
            JobSystem::ResetFrameStats();

            Time::Update();
            m_Window->OnUpdate();
            EventBus::ProcessEvents(BusType::MainThread);
            
            // Editor Begin
            Editor::BeginFrame();

            OnUpdate();
            AssetManager::Update();

            Editor::Render(); // Submits ImGui commands to ImGui internal buffers

            // Editor End (Generates DrawData for ImGui)
            Editor::EndFrame();

            // Update and Render additional Platform Windows
            if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
            }

            if (!m_Window->IsMinimized())
            {
                {
                    LH_PROFILE_SCOPE("Systems::Update");
                    Systems::Update<TransformSystem>();
                    Systems::Update<RenderingSystem>();
                }
            }
        }

        OnShutdown();
        Close();
    }

    void App::Close()
    {
		Editor::Shutdown();
		Systems::Shutdown();
        
        // Renderer::Shutdown(); // Phase 3

        AssetManager::Shutdown();
        AssetDatabase::Shutdown();
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
        if (e.GetWidth() == 0 || e.GetHeight() == 0)
            return;

        Renderer::OnResize(e.GetWidth(), e.GetHeight());
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

                // 2. Classify asset type
                AssetType resType = FileSystem::ClassifyFileType(srcPath);
                if (resType == AssetType::None) {
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
