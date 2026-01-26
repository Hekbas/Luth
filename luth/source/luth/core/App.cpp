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
#include "luth/core/JobSystemTests.h" // Include Tests
#include "luth/core/Profiler.h"
#include "luth/renderer/Renderer.h"
#include "luth/core/IOThread.h"

namespace Luth
{
    App::App(int argc, char** argv)
    {
        // Core Systems Init
        JobSystem::Init();
        IOThread::Init();
        m_FrameData.Init();
        
        // Run Job System Verification Tests
        JobSystem::Tests::RunAll();
        
        FileSystem::Init();
        AssetDatabase::Init(FileSystem::AssetsPath());
        AssetManager::Init();

        // Import Phase: Process any stale assets found by AssetDatabase
        const auto& dirtyAssetsRef = AssetDatabase::GetDirtyAssets();
        if (!dirtyAssetsRef.empty())
        {
            std::vector<UUID> assetsToImport = dirtyAssetsRef;
            
            LH_CORE_INFO("Importing {0} assets in parallel...", assetsToImport.size());
            
            JobSystem::Counter importCounter;
            
            JobSystem::Dispatch((u32)assetsToImport.size(), 1, [](JobSystem::JobArgs args) {
                std::vector<UUID>* assets = (std::vector<UUID>*)args.data;
                AssetManager::Import((*assets)[args.jobIndex]);
            }, &assetsToImport, &importCounter);

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

        // Temporary Loop for Phase 1 Testing
        // We are not using the full fiber loop yet as FrameContext is incomplete
        while (m_Running)
        {
            Time::Update();
            m_Window->OnUpdate();
            EventBus::ProcessEvents(BusType::MainThread);
            
            if (m_Window->IsMinimized())
            {
                std::this_thread::yield();
                continue;
            }

            // 1. Begin Vulkan Frame (Acquire Image, Start Recording)
            if (Renderer::BeginFrame())
            {
                // 2. Editor Logic
                Editor::BeginFrame();
                OnUpdate();
                AssetManager::Update();
                Editor::Render(); 
                Editor::EndFrame(); // Records ImGui to CB

                // 3. Update Viewports (ImGui)
                if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
                {
                    ImGui::UpdatePlatformWindows();
                    ImGui::RenderPlatformWindowsDefault();
                }

                // 4. Game Logic
                Systems::Update<TransformSystem>();
                // Systems::Update<RenderingSystem>(); 

                // 5. End Vulkan Frame (Submit, Present)
                Renderer::EndFrame();
                
                // Advance Frame Data
                m_FrameData.Advance();
            }
        }

        OnShutdown();
        Close();
    }

    void App::Close()
    {
		Editor::Shutdown();
		Systems::Shutdown();
        
        AssetManager::Shutdown();
        AssetDatabase::Shutdown();
        m_FrameData.Shutdown();
        IOThread::Shutdown();
        JobSystem::Shutdown();
    }

    WindowSpec App::ParseCommandLineArgs(int argc, char** argv)
    {
        WindowSpec spec;
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
                if (!fs::exists(srcPath)) {
                    LH_CORE_ERROR("Dropped file not found: {0}", srcPath.string());
                    continue;
                }

                AssetType resType = FileSystem::ClassifyFileType(srcPath);
                if (resType == AssetType::None) {
                    LH_CORE_WARN("Unsupported file type: {0}", srcPath.string());
                    continue;
                }

                fs::path destPath = FileSystem::GetPath(resType, srcPath.stem().string(), true);
                FileSystem::CreateDirectories(destPath.parent_path());
                fs::copy_file(srcPath, destPath, fs::copy_options::overwrite_existing);
                LH_CORE_INFO("Imported {0} to {1}", srcPath.filename().string(), destPath.string());
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
