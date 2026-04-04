#include "luthpch.h"
#include "luth/core/App.h"

#include "luth/platform/Window.h"
#include "luth/platform/Input.h"
#include "luth/platform/Event.h"
#include "luth/platform/AppEvent.h"
#include "luth/core/ProjectFile.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/MetaFile.h"
#include "luth/editor/Editor.h"
#include "luth/editor/ProjectLauncher.h"
#include "luth/editor/panels/ScenePanel.h"
#include "luth/editor/panels/ProjectPanel.h"
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
        AssetDatabase::InitEngine(FileSystem::EngineAssetsPath());
        AssetManager::Init();

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
        Systems::Init();
        Systems::SetScene(m_Scene.get());

        // 5. Editor + Launcher
        Editor::Init(m_Window.get());
        Editor::SetActiveScene(m_Scene);
        ProjectLauncher::Init();

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
                Editor::ShowProjectLauncher();
            }
        }
        else
        {
            // No project specified — show the launcher
            LH_CORE_INFO("No project specified -- showing Project Launcher");
            Editor::ShowProjectLauncher();
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
            if (ProjectLauncher::HasPendingProject())
            {
                LoadProject(ProjectLauncher::ConsumePendingProject());
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
            }

            // ── Step 3: Begin Vulkan Frame ──
            Renderer::BeginFrame(frameIndex);

            currentFrame.Reset();
            currentFrame.Params.DeltaTime = Time::DeltaTime();
            currentFrame.Params.TotalTime = Time::GetTime();
            currentFrame.Params.FrameNumber = frameIndex;

            // ── Step 4: Game Logic + Editor ──
            Editor::BeginFrame();
            OnUpdate();

            // Only update project-dependent systems when a project is loaded
            if (m_ProjectLoaded)
            {
                AssetManager::Update();
                AssetDatabase::ProcessPendingChanges();
            }

            Editor::Render();
            Editor::EndFrame();

            if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
            {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
            }

            // Scene systems always run (RenderingSystem must present the swapchain)
            Systems::Update<TransformSystem>();
            Systems::Update<AnimationSystem>();
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

    // ================================================================
    // Shutdown
    // ================================================================

    void App::Close()
    {
        Renderer::WaitForGPU();

		Editor::Shutdown();
		Systems::Shutdown();

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
            Editor::SaveSettings();
            AssetDatabase::UnloadProject();
            if (m_Scene) m_Scene->Clear();
        }

        // Set the project root in FileSystem
        FileSystem::SetProjectRoot(project.ProjectRoot);

        // Scan project assets
        AssetDatabase::LoadProject(FileSystem::AssetsPath());

        // Import dirty assets
        ImportDirtyAssets();

        // Start watching for file changes
        AssetDatabase::StartWatching();

        // Refresh the editor for the new project
        Editor::OnProjectChanged();

        // Track in recent projects and hide launcher
        ProjectLauncher::AddRecent(project.Name, project.FilePath);
        ProjectLauncher::Hide();

        m_ProjectLoaded = true;
        LH_CORE_INFO("Project loaded: '{}'", project.Name);
    }

    void App::ImportDirtyAssets()
    {
        const auto& dirtyAssets = AssetDatabase::GetDirtyAssets();
        if (dirtyAssets.empty()) return;

        std::vector<UUID> assetsToImport = dirtyAssets;
        LH_CORE_INFO("Importing {} assets...", assetsToImport.size());

        JobSystem::Counter importCounter(0);
        JobSystem::Dispatch((u32)assetsToImport.size(), 1, [](JobSystem::JobArgs args) {
            std::vector<UUID>* assets = (std::vector<UUID>*)args.data;
            AssetManager::Import((*assets)[args.jobIndex]);
        }, &assetsToImport, &importCounter);
        JobSystem::WaitForCounter(&importCounter);
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

    // ================================================================
    // File Drop Handling
    // ================================================================

    static bool IsImageExtension(const fs::path& ext)
    {
        static const std::unordered_set<std::string> s_Exts = {
            ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".tiff", ".tif"
        };
        std::string lower = ext.string();
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        return s_Exts.contains(lower);
    }

    void App::OnFileDrop(FileDropEvent& e)
    {
        for (const auto& srcPath : e.GetPaths()) {
            // Handle .luthproj drops — switch project
            if (srcPath.extension() == ".luthproj")
            {
                ProjectLauncher::AddRecent(srcPath.stem().string(), srcPath);
                ProjectLauncher::SetPendingProject(srcPath);
                return;
            }
        }

        // Regular asset drops require a loaded project
        if (!m_ProjectLoaded) return;

        auto* panel = Editor::GetPanel<ProjectPanel>();
        fs::path destDir = panel ? panel->GetCurrentDirectory() : FileSystem::AssetsPath();

        for (const auto& srcPath : e.GetPaths()) {
            if (srcPath.extension() == ".luthproj") continue; // Already handled

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

                fs::path destPath = destDir / srcPath.filename();
                FileSystem::CreateDirectories(destDir);
                fs::copy_file(srcPath, destPath, fs::copy_options::overwrite_existing);
                LH_CORE_INFO("Imported {0} to {1}", srcPath.filename().string(), destPath.string());

                if (resType == AssetType::Model) {
                    fs::path texDestDir = destDir / (srcPath.stem().string() + "_Textures");
                    fs::path srcDir = srcPath.parent_path();

                    std::vector<fs::path> scanDirs = { srcDir };
                    static const char* k_SiblingDirs[] = {
                        "textures", "Textures", "texture", "Texture",
                        "tex",      "Tex",      "maps",   "Maps",
                        "images",   "Images"
                    };
                    for (const char* sub : k_SiblingDirs) {
                        fs::path candidate = srcDir / sub;
                        if (fs::exists(candidate) && fs::is_directory(candidate))
                            scanDirs.push_back(candidate);
                    }

                    bool copiedAny = false;
                    for (const auto& dir : scanDirs) {
                        for (const auto& entry : fs::directory_iterator(dir)) {
                            if (!entry.is_regular_file()) continue;
                            if (!IsImageExtension(entry.path().extension())) continue;

                            if (!copiedAny) {
                                fs::create_directories(texDestDir);
                                copiedAny = true;
                            }

                            fs::path imgDest = texDestDir / entry.path().filename();
                            if (!fs::exists(imgDest))
                                fs::copy_file(entry.path(), imgDest, fs::copy_options::skip_existing);
                        }
                    }
                    if (copiedAny)
                        LH_CORE_INFO("Copied adjacent textures to {0}", texDestDir.filename().string());
                }

                UUID newUuid = MetaFile::Create(destPath, resType);
                AssetDatabase::RegisterAsset(destPath, newUuid, resType);

                if (AssetManager::HasImporter(resType))
                    AssetManager::Import(newUuid);

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
