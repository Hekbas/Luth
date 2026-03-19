#include "luthpch.h"
#include "luth/core/App.h"

#include "luth/platform/Window.h"
#include "luth/platform/Input.h"
#include "luth/platform/Event.h"
#include "luth/platform/AppEvent.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/MetaFile.h"
#include "luth/editor/Editor.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/scene/Systems.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/scene/systems/TransformSystem.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/Profiler.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/ShaderLibrary.h"
#include "luth/jobs/IOThread.h"
#include "luth/memory/MemoryTracker.h"

namespace Luth
{
    App::App(int argc, char** argv)
    {
        // Core Systems Init
        Memory::MemoryTracker::Init();
        JobSystem::Init();
        IOThread::Init();
        m_FrameData.Init();
        
        FileSystem::Init();
        AssetDatabase::Init(FileSystem::AssetsPath());
        AssetManager::Init();

        // Import Phase: Process any stale assets found by AssetDatabase
        const auto& dirtyAssetsRef = AssetDatabase::GetDirtyAssets();
        if (!dirtyAssetsRef.empty())
        {
            std::vector<UUID> assetsToImport = dirtyAssetsRef;
            
            LH_CORE_INFO("Importing {0} assets in parallel...", assetsToImport.size());
            
            JobSystem::Counter importCounter(0);
            
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
        Renderer::SetFrameData(&m_FrameData);
        ShaderLibrary::Init();
        
        // Scene & Systems
        m_Scene = std::make_shared<Scene>();
        Systems::Init();
        Systems::SetScene(m_Scene.get());

        Editor::Init(m_Window.get());
        Editor::SetActiveScene(m_Scene);

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

    // ===============================================================================
    // Pipelined Engine Loop (V2: Main Thread Isolated)
    // ===============================================================================
    // Structure (target):
    //   1. glfwPollEvents()         — OS message pump (main-thread-only)
    //   2. TryReclaimGPU(N-2)       — Non-blocking GPU completion check
    //   3. KickGame(Frame[N])       — Dispatch game logic to workers
    //   4. WaitForCounter(GameReady) — Main thread busy-spins (V2 isolated)
    //   5. KickRender(Frame[N-1])   — Dispatch render recording to workers
    //   6. WaitForCounter(RenderReady) — Wait for recording
    //   7. Submit(Frame[N-1])       — Send command buffers to GPU
    //   8. Present()                — Swapchain present (main-thread-only)
    //
    // Phase 2 Implementation: Frame data flow is correct. Full job parallelism
    // between Game(N) and Render(N-1) is wired structurally but runs sequentially
    // within each frame until Phase 3 (Render Graph) enables proper parallel recording.

    void App::Run()
    {
        OnInit();

        while (m_Running)
        {
            LH_PROFILE_FRAME("MainThread");
            
            u64 frameIndex = m_FrameData.GetFrameIndex();

            // ── Step 1: OS Message Pump (Main thread only, V2) ──
            Time::Update();
            m_Window->OnUpdate();
            EventBus::ProcessEvents(BusType::MainThread);
            
            if (m_Window->IsMinimized())
            {
                std::this_thread::yield();
                continue;
            }

            // ── Step 2: GPU Reclaim (N-2) ──
            // For the first few frames, there's nothing to reclaim
            FrameContext& currentFrame = m_FrameData.Current();
            if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
            {
                FrameContext& gpuFrame = m_FrameData.GPU();
                // V6: Check if GPU(N-2) is done. If not, current frame uses overflow.
                // For now, the blocking wait in AcquireImage handles this.
                // When PollerJobs are wired (Phase 3+), this becomes non-blocking.
            }

            // ── Step 3: Begin Vulkan Frame ──
            // Acquires swapchain image, waits on timeline for this slot's previous use
            Renderer::BeginFrame(frameIndex);

            // Reset frame resources now that GPU is done with this slot
            currentFrame.Reset();
            currentFrame.Params.DeltaTime = Time::DeltaTime();
            currentFrame.Params.TotalTime = Time::GetTime();
            currentFrame.Params.FrameNumber = frameIndex;

            // ── Step 4: Game Logic ──
            Editor::BeginFrame();
            OnUpdate();
            AssetManager::Update();
            Editor::Render(); 
            Editor::EndFrame();

            if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
            }

            Systems::Update<TransformSystem>();
            Systems::Update<RenderingSystem>();

            // ── Step 5: End Frame (Submit + Present) ──
            Renderer::EndFrame();
            
            // ── Step 6: Advance Frame ──
            m_FrameData.Advance();
            JobSystem::ResetFrameStats();
        }

        OnShutdown();
        Close();
    }

    void App::Close()
    {
        // Wait for all GPU work to finish before destroying any resources
        Renderer::WaitForGPU();

		Editor::Shutdown();
		Systems::Shutdown();

        AssetManager::Shutdown();
        AssetDatabase::Shutdown();
        ShaderLibrary::Shutdown();

        // Flush deferred GPU resource deletions queued by asset/shader destructors
        Renderer::FlushDeletionQueues();

        Renderer::Shutdown();

        // Destroy the window after Vulkan (surface must outlive swapchain),
        // but Editor::Shutdown already removed ImGui callbacks so no stale dispatch.
        if (m_Window) {
            m_Window->Shutdown();
        }

        m_FrameData.Shutdown();
        IOThread::Shutdown();
        JobSystem::Shutdown();
        Memory::MemoryTracker::Shutdown();
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
