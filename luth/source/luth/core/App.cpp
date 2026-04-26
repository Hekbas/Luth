#include "luthpch.h"
#include "luth/core/App.h"

#include "luth/platform/Window.h"
#include "luth/platform/Input.h"
#include "luth/events/Event.h"
#include "luth/events/AppEvent.h"
#include "luth/core/ProjectFile.h"
#include "luth/core/RenderSnapshot.h"
#include "luth/core/Version.h"
#include "luth/core/EditorHooks.h"
#include "luth/resources/FileSystem.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/scene/systems/TransformSystem.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/PickingSystem.h"
#include "luth/jobs/JobSystem.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/PipelineCache.h"
#include "luth/jobs/IOThread.h"
#include "luth/memory/MemoryTracker.h"
#include "luth/scene/systems/AnimationSystem.h"

namespace Luth
{
    // ================================================================
    // Engine Root Discovery
    // ================================================================

    static fs::path DiscoverEngineRoot()
    {
        fs::path dir = fs::current_path();

        for (int i = 0; i < 6; ++i)
        {
            fs::path candidate = dir / "luth";
            if (fs::exists(candidate) && fs::is_directory(candidate)
                && fs::exists(candidate / "assets"))
            {
                return candidate;
            }
            if (!dir.has_parent_path() || dir == dir.parent_path())
                break;
            dir = dir.parent_path();
        }

        LH_CORE_WARN("Could not discover engine root, falling back to CWD");
        return fs::current_path();
    }

    // ================================================================
    // Phase 1: Engine Boot (no project needed)
    // ================================================================

    App::App(int argc, char** argv)
    {
        // 1. Core systems
        Memory::MemoryTracker::Init();
        JobSystem::Init();
        IOThread::Init();
        m_FrameData.Init();

        // 2. Engine root + engine assets
        fs::path engineRoot = DiscoverEngineRoot();
        FileSystem::InitEngine(engineRoot);
        AssetManager::Init();
        AssetDatabase::InitEngine(FileSystem::EngineAssetsPath());
        AssetManager::ImportDirty();
        AssetDatabase::ClearDirtyAssets();

        // 3. Window + Renderer
        WindowSpec ws = ParseCommandLineArgs(argc, argv);
        SetAppTitle(ws);
        m_Window = Window::Create(ws);
        Input::Init();

        Renderer::Init(m_Window->GetNativeWindow());
        Renderer::SetFrameData(&m_FrameData);
        ShaderLibrary::Init();

        // 4. Scene & Systems (RenderingSystem loads engine shaders — no project needed)
        m_Scene = std::make_shared<Scene>();
        SystemRegistry::Init();
        SystemRegistry::SetScene(m_Scene.get());

        // 5. Editor + Launcher (no-op in runtime-only builds with no hooks registered)
        if (auto* h = EditorHooks::Get())
        {
            h->Init(m_Window.get());
            h->SetActiveScene(m_Scene);
        }

        // 6. Check CLI args for a .luthproj to open immediately
        fs::path projectHint;
        for (int i = 1; i < argc; ++i)
        {
            fs::path arg(argv[i]);
            if (arg.extension() == ".luthproj" || fs::is_directory(arg))
            {
                projectHint = arg;
                break;
            }
        }

        // Try to auto-discover project from CLI arg or CWD
        if (!projectHint.empty())
        {
            ProjectFile project;
            if (project.Discover(projectHint))
            {
                LoadProject(project.FilePath);
            }
            else
            {
                LH_CORE_WARN("CLI project hint not found: {}", projectHint.string());
                if (auto* h = EditorHooks::Get()) h->ShowProjectLauncher();
            }
        }
        else
        {
            // No project specified — show the launcher
            LH_CORE_INFO("No project specified -- showing Project Launcher");
            if (auto* h = EditorHooks::Get()) h->ShowProjectLauncher();
        }

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

            // ── Step 1: OS Message Pump ──
            Time::Update();
            m_Window->OnUpdate();
            EventBus::ProcessEvents(BusType::MainThread);

            // Check if user selected a project from launcher
            if (auto* h = EditorHooks::Get(); h && h->HasPendingProject())
            {
                LoadProject(h->ConsumePendingProject());
            }

            if (m_Window->IsMinimized())
            {
                std::this_thread::yield();
                continue;
            }

            // ── Step 2: GPU Reclaim (N-2) ──
            FrameContext& currentFrame = m_FrameData.Current();
            if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
            {
                FrameContext& gpuFrame = m_FrameData.GPU();
                (void)gpuFrame;
            }

            // ── Step 3: Begin Vulkan Frame ──
            Renderer::BeginFrame(frameIndex);

            currentFrame.Reset();
            currentFrame.Params.DeltaTime = Time::DeltaTime();
            currentFrame.Params.TotalTime = Time::GetTime();
            currentFrame.Params.FrameNumber = frameIndex;

            // ── Step 4: Editor + Asset Update (must precede stage dispatch) ──
            if (auto* h = EditorHooks::Get()) h->BeginFrame();
            OnUpdate();

            // Only update project-dependent systems when a project is loaded
            if (m_ProjectLoaded)
            {
                AssetManager::Update();
                AssetDatabase::ProcessPendingChanges();
                AssetManager::ImportDirty();
                AssetDatabase::ClearDirtyAssets();
            }

            if (auto* h = EditorHooks::Get())
            {
                h->Render();
                h->EndFrame();
            }

            // Snapshot editor state once per frame — reused for camera setup
            // and play-mode system gating below. Headless runtime (no editor
            // hook) leaves defaults: hasCamera=false, playState=Editing,
            // previewAnimationInEditor=true → game systems always tick.
            EditorViewportState viewState;
            bool haveEditor  = false;
            PlayState playState = PlayState::Editing;
            bool stepThisFrame = false;
            if (auto* h = EditorHooks::Get())
            {
                h->GetViewportState(viewState);
                playState     = h->GetPlayState();
                stepThisFrame = h->ConsumeStepRequest();
                haveEditor    = true;
            }

            // Feed camera/editor state into RenderingSystem before its Update
            if (auto rs = SystemRegistry::GetSystem<RenderingSystem>())
            {
                CameraParams cp;
                if (haveEditor)
                {
                    if (viewState.hasCamera)
                    {
                        cp.view       = viewState.view;
                        cp.projection = viewState.projection;
                        cp.position   = viewState.position;
                        cp.nearZ      = viewState.nearZ;
                        cp.farZ       = viewState.farZ;
                    }
                    cp.iblIntensity     = viewState.iblIntensity;
                    cp.skyboxIntensity  = viewState.skyboxIntensity;
                    cp.selectedEntities = std::move(viewState.selectedEntities);
                }
                rs->SetCameraParams(cp);
            }

            // Game systems tick only in Playing, Paused+Step, or Editing when
            // the preview toggle is on. Standalone runtime (no editor) always
            // ticks them. Read by GameStageFn through m_RunGameSystems.
            m_RunGameSystems = !haveEditor
                || (playState == PlayState::Playing)
                || (playState == PlayState::Paused && stepThisFrame)
                || (playState == PlayState::Editing && viewState.previewAnimationInEditor);

            // ── Step 5: Stage Dispatch — pipelined Game(N) | Render(N-1) ──
            // Steady (N >= 2): Render of Previous() dispatched *before* the
            // GameReady wait, so Game(N) and Render(N-1) run concurrently on
            // separate worker fibers. This is the parallelism that v2.8.4
            // delivers; comment block above is now accurate.
            //
            // Sync warm-up (N < 2): pipeline is cold — no Previous() yet at
            // N=0, and we want the first visible frame to show fresh data
            // rather than 1-frame-stale Previous. So Render targets Current()
            // and waits inline. Steady kicks in from N=2 onward.
            //
            // Spec D5 nominally has frame 0 *skip render entirely*; we keep
            // it as a sync render instead because skipping submit at iter 0
            // breaks Vulkan timeline + imageAvailable-semaphore math, which
            // would require backend-side init tweaks beyond App.cpp's scope
            // for this sub-task. End state (steady from N=2) is identical.
            const bool isSteady = (frameIndex >= 2);

            JobSystem::Execute(GameStageFn, this, &currentFrame.GameReady, "GameStage");

            FrameContext* renderFrame = nullptr;
            if (isSteady)
            {
                renderFrame = &m_FrameData.Previous();
                m_FrameData.SetRenderFrameIndex(frameIndex - 1);
                JobSystem::Execute(RenderStageFn, this, &renderFrame->RenderReady, "RenderStage");
            }

            JobSystem::WaitForCounter(&currentFrame.GameReady);

            if (isSteady)
            {
                JobSystem::WaitForCounter(&renderFrame->RenderReady);
            }
            else
            {
                renderFrame = &currentFrame;
                m_FrameData.SetRenderFrameIndex(frameIndex);
                JobSystem::Execute(RenderStageFn, this, &renderFrame->RenderReady, "RenderStage");
                JobSystem::WaitForCounter(&renderFrame->RenderReady);
            }

            // ── Step 6: Mouse picking + Frame end (main thread) ──
            // PickingSystem reads back the EntityID texture written by the
            // render stage, so it must follow RenderReady. EndFrame is a
            // no-op today (Submit + Present happens inside the render stage).
            SystemRegistry::Update<PickingSystem>();
            Renderer::EndFrame();

            // ── Step 7: Advance Frame ──
            m_FrameData.Advance();
            JobSystem::ResetFrameStats();
        }

        OnShutdown();
        Close();
    }

    // ────────────────────────────────────────────────────────────────────
    // Stage entry points
    // ────────────────────────────────────────────────────────────────────

    void App::GameStageFn(JobSystem::JobArgs args)
    {
        auto* app = static_cast<App*>(args.data);

        SystemRegistry::Update<TransformSystem>();
        if (app->m_RunGameSystems)
            SystemRegistry::Update<AnimationSystem>();

        // End-of-game-stage capture: ECS is coherent, snapshot is fresh.
        FrameContext& cf = Renderer::GetFrameData()->Current();
        CaptureSnapshot(*app->m_Scene, cf.LogicMemory, cf.Snapshot);
    }

    void App::RenderStageFn(JobSystem::JobArgs args)
    {
        (void)args;  // App* unused for now; S8 will read m_FrameData via it
        SystemRegistry::Update<RenderingSystem>();
    }

    // ================================================================
    // Shutdown
    // ================================================================

    void App::Close()
    {
        Renderer::WaitForGPU();

        // Persist the Vulkan pipeline cache to the active project before tearing
        // down systems / the renderer. SaveToProject is a no-op if no project.
        PipelineCache::SaveToProject();

        if (auto* h = EditorHooks::Get()) h->Shutdown();
        SystemRegistry::Shutdown();

        AssetManager::Shutdown();
        AssetDatabase::Shutdown();
        ShaderLibrary::Shutdown();

        if (m_Scene) m_Scene->Clear();

        Renderer::FlushDeletionQueues();
        Renderer::Shutdown();

        if (m_Window) {
            m_Window->Shutdown();
        }

        m_FrameData.Shutdown();
        IOThread::Shutdown();
        JobSystem::Shutdown();
        Memory::MemoryTracker::Shutdown();
    }

    // ================================================================
    // Project Loading
    // ================================================================

    void App::LoadProject(const fs::path& luthprojPath)
    {
        ProjectFile project;
        if (!project.Load(luthprojPath))
        {
            LH_CORE_ERROR("Failed to load project: {}", luthprojPath.string());
            return;
        }

        LH_CORE_INFO("Loading project '{}' at '{}'", project.Name, project.ProjectRoot.string());

        // If switching away from an existing project, clean up first
        if (m_ProjectLoaded)
        {
            if (auto* h = EditorHooks::Get()) h->SaveSettings();
            PipelineCache::SaveToProject();
            if (auto rs = SystemRegistry::GetSystem<RenderingSystem>())
                rs->OnProjectUnloaded();
            AssetDatabase::UnloadProject();
            if (m_Scene) m_Scene->Clear();
        }

        // Set the project root in FileSystem
        FileSystem::SetProjectRoot(project.ProjectRoot);

        // Load any persisted Vulkan pipeline cache for this project
        PipelineCache::LoadFromProject();

        // Scan project assets
        AssetDatabase::LoadProject(FileSystem::AssetsPath());

        // Import dirty assets
        AssetManager::ImportDirty();

        // Start watching for file changes
        AssetDatabase::StartWatching();

        // Refresh the editor for the new project
        if (auto* h = EditorHooks::Get()) h->OnProjectChanged();

        // Notify systems that depend on project paths (e.g. shader hot-reload watcher)
        if (auto rs = SystemRegistry::GetSystem<RenderingSystem>())
            rs->OnProjectLoaded();

        // Track in recent projects and hide launcher
        if (auto* h = EditorHooks::Get())
        {
            h->AddRecentProject(project.Name, project.FilePath);
            h->HideProjectLauncher();
        }

        m_ProjectLoaded = true;
        LH_CORE_INFO("Project loaded: '{}'", project.Name);
    }

    // ================================================================
    // Utility
    // ================================================================

    WindowSpec App::ParseCommandLineArgs(int argc, char** argv)
    {
        WindowSpec spec;
        return spec;
    }

    void App::SetAppTitle(WindowSpec& ws)
    {
		std::string title = Luth::GetFullTitleString();

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

    // ================================================================
    // File Drop Handling
    // ================================================================

    void App::OnFileDrop(FileDropEvent& e)
    {
        for (const auto& srcPath : e.GetPaths()) {
            if (srcPath.extension() == ".luthproj")
            {
                if (auto* h = EditorHooks::Get())
                {
                    h->AddRecentProject(srcPath.stem().string(), srcPath);
                    h->SetPendingProject(srcPath);
                }
                return;
            }
        }

        if (!m_ProjectLoaded) return;

        fs::path destDir = FileSystem::AssetsPath();
        if (auto* h = EditorHooks::Get())
        {
            auto dir = h->GetProjectCurrentDir();
            if (!dir.empty()) destDir = dir;
        }

        for (const auto& srcPath : e.GetPaths()) {
            if (srcPath.extension() == ".luthproj") continue;
            AssetDatabase::IngestFile(srcPath, destDir);
        }
    }
}
