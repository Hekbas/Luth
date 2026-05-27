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
#include "luth/core/DebugDraw.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/Image.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/resources/AssetManager.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/scene/systems/TransformSystem.h"
#include "luth/scene/systems/PlayerControllerSystem.h"
#include "luth/scene/systems/PhysicsSystem.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/systems/PickingSystem.h"
#include "luth/jobs/JobSystem.h"
#include "luth/jobs/MainThreadPump.h"
#include "luth/core/diagnostics/Profiler.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/shader/ShaderLibrary.h"
#include "luth/renderer/backend/vulkan/PipelineCache.h"
#include "luth/jobs/IOThread.h"
#include "luth/memory/MemoryTracker.h"
#include "luth/memory/TaggedPageAllocator.h"
#include "luth/scene/systems/AnimationSystem.h"
#include "luth/core/time/Timer.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

namespace Luth
{
    // ── Engine Root Discovery ──

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

    // ── Phase 1: Engine Boot — no project needed ──

    App::App(int argc, char** argv)
    {
        // 1. Core systems
        Memory::MemoryTracker::Init();
        Memory::TaggedPageAllocator::Get().Init();
        JobSystem::Init();

        // Jolt global state must come up before SystemRegistry::Init constructs PhysicsSystem (which
        // builds a JPH::PhysicsSystem in its ctor). Factory + RegisterTypes register Jolt's serializable
        // types and collision dispatch tables — required before any body creation.
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();

        IOThread::Init();
        m_FrameData.Init();
        DebugDraw::Init();
        Image::Init();   // sets stb's global flip flag to 0; no other site touches it

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

    // ── Pipelined Engine Loop — Game(N) | Render(N-1) | GPU(N-2) ──
    // invariant: see arch/frame-pipeline.md for the full pipeline diagram and ring-slot ownership.
    // Main thread is V2-isolated: it pumps OS events, drives editor ImGui, dispatches the two
    // stage fibers, and busy-spins on their counters — never steals worker jobs.
    //
    // Frames 0/1 run synchronously (game then render against Current()) to seed the pipeline.
    // From frame 2 onward, Render of Previous() is dispatched before the GameReady wait, so
    // Game(N) and Render(N-1) overlap on separate worker fibers.
    // The frame boundary is the RenderSnapshot captured at end of game stage; render reads it
    // from a different FrameContext slot, so the two stages never race on the same data.

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
            MainThreadPump::Drain();

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
            // Skip on swapchain rebuild (resize / DPI change); same frameIndex retries (Advance is at end of loop).
            if (!Renderer::BeginFrame(frameIndex))
            {
                std::this_thread::yield();
                continue;
            }

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
                    cp.enableVolumetricFog = viewState.enableVolumetricFog;
                    cp.selectedEntities = std::move(viewState.selectedEntities);

                    cp.outlineColor          = viewState.outlineColor;
                    cp.outlineWidth          = viewState.outlineWidth;
                    cp.outlineOccludedAlpha  = viewState.outlineOccludedAlpha;

                    cp.gridAxisXColor    = viewState.gridAxisXColor;
                    cp.gridAxisZColor    = viewState.gridAxisZColor;
                    cp.gridColor         = viewState.gridColor;
                    cp.gridMajorScale    = viewState.gridMajorScale;
                    cp.gridFadeStart     = viewState.gridFadeStart;
                    cp.gridFadeEnd       = viewState.gridFadeEnd;
                    cp.gridLineThickness = viewState.gridLineThickness;
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
            // Frames 0/1 sync-render Current() to seed the pipeline; from
            // frame 2 on, Render(N-1) is dispatched before the GameReady
            // wait so it overlaps with Game(N) on separate worker fibers.
            const bool isSteady = (frameIndex >= 2);

            // Cycle DebugDraw's producer slot before the Game stage spawns. Slot = frameIndex mod 2;
            // Render(N-1) reads the previous slot, which was filled by Game(N-1) and is stable until
            // BeginGameFrame is called for that index again two frames out.
            DebugDraw::BeginGameFrame(frameIndex);

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

    // ── Stage entry points ──

    void App::GameStageFn(JobSystem::JobArgs args)
    {
        // Stage tag propagates to sub-jobs dispatched from this fiber so
        // stage-isolated subsystems (MaterialSystem, BoneMatrixBuffer) can
        // assert their callers are on the right stage.
        JobSystem::SetCurrentStage(JobSystem::Stage::Game);
        Timer stageTimer;

        auto* app = static_cast<App*>(args.data);

        SystemRegistry::Update<TransformSystem>();
        // Stub player input → CharacterController.desiredVelocity. Gated so authoring (Editing) mode
        // doesn't drive characters from WASD; PhysicsSystem still runs unconditionally so falling
        // bodies keep falling in editor preview.
        if (app->m_RunGameSystems)
            SystemRegistry::Update<PlayerControllerSystem>();
        SystemRegistry::Update<PhysicsSystem>();
        if (app->m_RunGameSystems)
            SystemRegistry::Update<AnimationSystem>();

        // CaptureSnapshot must run after all ECS mutations for the frame —
        // it freezes the state Render(N-1) will read next iteration.
        FrameContext& cf = Renderer::GetFrameData()->Current();
        CaptureSnapshot(*app->m_Scene, cf.LogicMemory, cf.Snapshot);

        JobSystem::RecordStageTime(JobSystem::Stage::Game, stageTimer.ElapsedMillis());
    }

    void App::RenderStageFn(JobSystem::JobArgs args)
    {
        JobSystem::SetCurrentStage(JobSystem::Stage::Render);
        Timer stageTimer;

        (void)args;
        SystemRegistry::Update<RenderingSystem>();

        JobSystem::RecordStageTime(JobSystem::Stage::Render, stageTimer.ElapsedMillis());
    }

    // ── Shutdown ──

    void App::Close()
    {
        Renderer::WaitForGPU();

        // Persist the Vulkan pipeline cache to the active project before tearing
        // down systems / the renderer. SaveToProject is a no-op if no project.
        PipelineCache::SaveToProject();

        if (auto* h = EditorHooks::Get()) h->Shutdown();
        SystemRegistry::Shutdown();

        // Jolt global teardown — must come after SystemRegistry::Shutdown so PhysicsSystem's destructor
        // (which holds a JPH::PhysicsSystem) finishes while Jolt globals are still alive.
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;

        AssetManager::Shutdown();
        AssetDatabase::Shutdown();
        ShaderLibrary::Shutdown();

        if (m_Scene) m_Scene->Clear();

        Renderer::FlushDeletionQueues();
        Renderer::Shutdown();

        if (m_Window) {
            m_Window->Shutdown();
        }

        DebugDraw::Shutdown();
        m_FrameData.Shutdown();
        IOThread::Shutdown();
        JobSystem::Shutdown();
        EventBus::FlushAll();
        Memory::TaggedPageAllocator::Get().Shutdown();
        Memory::MemoryTracker::Shutdown();
    }

    // ── Project Loading ──

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
            // Drain GPU before tearing down asset / scene state — pending
            // cmd buffers must not reference resources about to be destroyed.
            Renderer::WaitForGPU();

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

    // ── Utility ──

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

    // ── File Drop Handling ──

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
