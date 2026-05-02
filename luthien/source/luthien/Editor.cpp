#include "lepch.h"
#include "luthien/Editor.h"
#include "luthien/EditorSnapshot.h"
#include "luthien/events/EditorSignals.h"
#include "luth/events/EventBus.h"
#include "luth/jobs/JobSystem.h"
#include "luth/platform/WinWindow.h"
#include "luth/platform/FileDialog.h"
#include "luth/scene/systems/SystemRegistry.h"
#include "luth/scene/systems/RenderingSystem.h"
#include "luth/scene/SceneSerializer.h"
#include "luth/scene/Components.h"
#include "luth/renderer/Renderer.h"
#include "luth/renderer/RenderBackend.h"
#include "luth/resources/FileSystem.h"
#include "luth/resources/MetaFile.h"
#include "luth/resources/AssetDatabase.h"
#include "luth/resources/AssetManager.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/backend/vulkan/VulkanContext.h"
#include "luth/renderer/backend/vulkan/VulkanBackend.h"

#include "luthien/CommandHistory.h"
#include "luthien/inspectors/ComponentDrawerRegistry.h"
#include "luthien/inspectors/component_drawers/RegisterComponentDrawers.h"
#include "luthien/panels/HierarchyPanel.h"
#include "luthien/panels/InspectorPanel.h"
#include "luthien/panels/ProjectPanel.h"
#include "luthien/panels/ResourcePanel.h"
#include "luthien/panels/ScenePanel.h"
#include "luthien/panels/GamePanel.h"
#include "luthien/panels/RenderPanel.h"
#include "luthien/panels/ProfilerPanel.h"
#include "luthien/panels/FrameDebuggerPanel.h"
#include "luthien/panels/HistoryPanel.h"
#include "luthien/panels/TextureRemapDialog.h"
#include "luth/resources/importers/ModelImporter.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/EditorStyle.h"
#include "luth/core/Version.h"
#include "luthien/EditorSettings.h"
#include "luthien/widgets/Icons.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>

namespace Luth
{
    // Cached so Style → Save Current As can round-trip the live style.
    static std::optional<StyleFile> s_CurrentStyle;

    static void ApplyStyleFile(std::optional<StyleFile> sf)
    {
        if (!sf) return;
        EditorStyle::LoadFonts(sf->Font);
        EditorStyle::Apply(sf->Preset);
        s_CurrentStyle = std::move(sf);
    }

    static void ApplyBuiltinStyle(const char* name)           { ApplyStyleFile(EditorStyle::LoadBuiltin(name)); }
    static void ApplyStyleFromFile(const fs::path& path)      { ApplyStyleFile(EditorStyle::LoadFromFile(path)); }

    static bool PendingIsPath(const std::string& s)
    {
        return s.find('/') != std::string::npos || s.find('\\') != std::string::npos
            || (s.size() > 5 && s.compare(s.size() - 5, 5, ".json") == 0);
    }

    void Editor::Init(Window* window)
    {
        s_Window = window;
        LH_CORE_INFO("Initializing Luth Editor");

        // Settings must be loaded first so InitImGui can apply the persisted style,
        // which populates io.Fonts before the Vulkan font atlas is built.
        LoadSettings();
        InitImGui(window);
        ProjectLauncher::Init();
        InitPanels();
        ApplyPersistence();

        // Forward AssetDatabase file-watch flushes onto the EventBus as typed
        // AssetChangedSignals so panels (Project/Resource/Inspector/ThumbnailCache)
        // can react via subscriptions instead of polling. The current AssetDatabase
        // callback API only exposes the dirty-UUID list, not per-asset op — for
        // v2.9.1 we publish Modified for everything; subscribers that need to
        // distinguish import-vs-delete query AssetDatabase::Exists(uuid) themselves.
        AssetDatabase::AddChangeCallback([]() {
            for (const UUID& uuid : AssetDatabase::GetDirtyAssets()) {
                EventBus::Enqueue<AssetChangedSignal>(BusType::MainThread,
                    AssetChangedSignal::Op::Modified, uuid);
            }
        });

        // Replace v2.8.x hierarchy-version polling with reactive dirty-marking.
        // Every EntityCommand publishes HierarchyChangedSignal; this handler
        // bumps the dirty flag exactly when user edits land. Scene LOAD does
        // not fire signals (deserialization bypasses commands), so the dirty
        // flag stays clean across scene open/close — no need for the prior
        // s_LastHierarchyVersion stamp dance.
        EventBus::Subscribe<HierarchyChangedSignal>(BusType::MainThread,
            [](Event&) { Editor::MarkDirty(); });
    }

    void Editor::InitImGui(Window* window)
    {
        IMGUI_CHECKVERSION();
        s_Context = ImGui::CreateContext();
        LH_CORE_TRACE(" - Created ImGui context");

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        LH_CORE_TRACE(" - Enabled docking + multi-viewport");

        // Apply persisted style BEFORE CreateFontsTexture: LoadFonts populates
        // io.Fonts. Window-chrome colors belong here too since Matrix tints them.
        if (!s_Settings.activeStylePath.empty() && fs::exists(s_Settings.activeStylePath))
            ApplyStyleFromFile(s_Settings.activeStylePath);
        else
            ApplyBuiltinStyle(s_Settings.activeStyle.c_str());

        bool matrix = (s_CurrentStyle && s_CurrentStyle->Preset.Name == "Matrix");

        #ifdef _WIN32
            if (matrix)
                window->SetWindowColors({ 0, 4, 0 }, { 0, 255, 0 }, { 0, 255, 0 });
            else
                window->SetWindowColors({ 30, 31, 34 }, { 67, 69, 74 }, { 223, 225, 229 });
        #endif

        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan) {
            LH_CORE_TRACE(" - Initialized ImGui GLFW/Vulkan backend");
            // Callbacks installed manually in WinWindow for event routing.
            ImGui_ImplGlfw_InitForVulkan((GLFWwindow*)window->GetNativeWindow(), false);

            auto& ctx = VulkanContext::Get();
            auto* vkRenderer = static_cast<VulkanBackend*>(Renderer::GetBackend());

            // Dedicated pool: ImGui freely allocates/frees per-texture descriptor sets.
            VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2000 } };
            VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            pool_info.maxSets = 2000;
            pool_info.poolSizeCount = 1;
            pool_info.pPoolSizes = pool_sizes;
            vkCreateDescriptorPool(ctx.GetDevice(), &pool_info, nullptr, &s_ImGuiPool);

            ImGui_ImplVulkan_InitInfo init_info = {};
            init_info.Instance = ctx.GetInstance();
            init_info.PhysicalDevice = ctx.GetPhysicalDevice();
            init_info.Device = ctx.GetDevice();
            init_info.QueueFamily = ctx.GetGraphicsFamily();
            init_info.Queue = ctx.GetGraphicsQueue();
            init_info.PipelineCache = VK_NULL_HANDLE;
            init_info.DescriptorPool = s_ImGuiPool;
            init_info.Subpass = 0;
            init_info.MinImageCount = MAX_FRAMES_IN_FLIGHT;
            init_info.ImageCount = MAX_FRAMES_IN_FLIGHT;
            init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            init_info.Allocator = nullptr;
            init_info.CheckVkResultFn = nullptr;
            init_info.UseDynamicRendering = true;

            static const VkFormat swapChainFormat = vkRenderer->GetSwapchain().GetImageFormat();
            init_info.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
            init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapChainFormat;

            ImGui_ImplVulkan_Init(&init_info);
            ImGui_ImplVulkan_CreateFontsTexture();
        }
    }

    void Editor::InitPanels()
    {
        auto rs = SystemRegistry::GetSystem<RenderingSystem>();

        AddPanel(new HierarchyPanel());
        AddPanel(new InspectorPanel());
        AddPanel(new ProjectPanel());
        AddPanel(new ResourcePanel());
        AddPanel(new ScenePanel(rs));
        AddPanel(new GamePanel(rs));
        AddPanel(new RenderPanel());
        AddPanel(new ProfilerPanel());
        AddPanel(new FrameDebuggerPanel());
        AddPanel(new HistoryPanel());

        ComponentDrawers::RegisterComponentDrawers();

        for (auto& panel : s_Panels)
            panel->OnInit();
    }

    void Editor::ApplyPersistence()
    {
        if (auto* sp = GetPanel<ScenePanel>()) {
            sp->GetEditorCamera().ApplySettings(s_Settings);
            sp->SetShowControlsOverlay(s_Settings.showControlsOverlay);
        }
        if (auto* pp = GetPanel<ProjectPanel>())
            pp->SetThumbnailSize(s_Settings.thumbnailSize);

        if (s_Settings.skyboxPath != "textures/environment.hdr" && !s_Settings.skyboxPath.empty()) {
            fs::path skyboxAbsPath = fs::path(s_Settings.skyboxPath).is_absolute()
                ? fs::path(s_Settings.skyboxPath)
                : FileSystem::ResolveAsset(s_Settings.skyboxPath);
            if (fs::exists(skyboxAbsPath))
                SystemRegistry::GetSystem<RenderingSystem>()->ReloadSkybox(skyboxAbsPath);
        }
    }

    void Editor::Shutdown()
    {
        // Sync panel state → settings before saving
        if (auto* sp = GetPanel<ScenePanel>()) {
            sp->GetEditorCamera().SyncToSettings(s_Settings);
            s_Settings.showControlsOverlay = sp->GetShowControlsOverlay();
        }
        if (auto* pp = GetPanel<ProjectPanel>())
            s_Settings.thumbnailSize = pp->GetThumbnailSize();
        s_Settings.lastSceneUUID = s_ScenePath.empty() ? "" : AssetDatabase::GetUUID(s_ScenePath).ToString();

        SaveSettings();

        LH_CORE_TRACE("Cleaning up {} panels", s_Panels.size());
        ComponentDrawerRegistry::Shutdown();
        s_PanelRegistry.clear();
        s_Panels.clear();
        UI::ClearTextureCache();

        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan) {
            // Clear GLFW callbacks that forward to ImGui BEFORE destroying
            // the backend. We installed these manually (install_callbacks=false),
            // so ImGui_ImplGlfw_Shutdown won't remove them. Without this,
            // Win32 focus messages during Vulkan teardown dispatch into freed
            // ImGui backend data.
            if (s_Window) {
                GLFWwindow* win = (GLFWwindow*)s_Window->GetNativeWindow();
                if (win) {
                    glfwSetWindowFocusCallback(win, nullptr);
                    glfwSetCursorEnterCallback(win, nullptr);
                    glfwSetKeyCallback(win, nullptr);
                    glfwSetMouseButtonCallback(win, nullptr);
                    glfwSetScrollCallback(win, nullptr);
                    glfwSetCursorPosCallback(win, nullptr);
                    glfwSetCharCallback(win, nullptr);
                }
            }

            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();

            if (s_ImGuiPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(VulkanContext::Get().GetDevice(), s_ImGuiPool, nullptr);
                s_ImGuiPool = VK_NULL_HANDLE;
            }
        }
        
        s_Context = nullptr;
        s_Window = nullptr;
        s_ActiveScene.reset();
        LH_CORE_INFO("Editor system shutdown completed");
    }

    void Editor::BeginFrame()
    {
        // Apply deferred style change (fonts can't be rebuilt between NewFrame/Render)
        if (!s_PendingStyle.empty()) {
            if (PendingIsPath(s_PendingStyle)) {
                ApplyStyleFromFile(s_PendingStyle);
                s_Settings.activeStylePath = s_PendingStyle;
                s_Settings.activeStyle     = s_CurrentStyle ? s_CurrentStyle->Preset.Name : "";
            } else {
                ApplyBuiltinStyle(s_PendingStyle.c_str());
                s_Settings.activeStyle = s_PendingStyle;
                s_Settings.activeStylePath.clear();
            }
            s_PendingStyle.clear();

            if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan)
                ImGui_ImplVulkan_CreateFontsTexture();
        }

        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan) {
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
        }
    }

    void Editor::EndFrame()
    {
        ImGuiIO& io = ImGui::GetIO();

        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan) {
            ImGui::Render();

            // Update/render detached platform windows.
            if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
            }

            // Actual ImGui draw calls run through RenderingSystem::AddImGuiPass
            // inside the RenderGraph; EndFrame just finalizes the frame.
        }
    }

    void Editor::GatherJobThunk(JobSystem::JobArgs args)
    {
        Panel* panel = static_cast<Panel*>(args.data);
        LH_PROFILE_SCOPE_DYNAMIC_CSTR(panel->GetWindowID());

        // Reset the panel's scratch first thing — Reset() rewinds the bump pointer
        // without freeing pages, so the prior frame's m_SnapshotFragment is invalid
        // from this line onward. Null it explicitly so a thrown OnGather doesn't
        // leave a dangling pointer for the snapshot-assembly phase to read.
        panel->m_GatherAlloc.Reset();
        panel->m_SnapshotFragment = nullptr;
        panel->m_FragmentType = std::type_index(typeid(void));

        EditorSnapshotBuilder builder(*panel);
        try
        {
            panel->OnGather(builder);
        }
        catch (const std::exception& e)
        {
            // Pillar 5 (editor-console-errors v2.9.2) extends with stack-trace dump
            // to the console panel. For v2.9.0 we just log and bump the crash streak.
            LH_CORE_ERROR("Panel '{}' threw in OnGather: {}", panel->GetWindowID(), e.what());
            panel->m_SnapshotFragment = nullptr;
            if (++panel->m_CrashStreak >= 3) panel->m_Crashed = true;
        }
    }

    void Editor::Render()
    {
        LH_PROFILE_FUNCTION();

        // Skip rendering if context is null (Vulkan case)
        if (!s_Context) return;

        // ── Gather phase ─────────────────────────────────────────────────────────
        // Dispatch one gather job per visible panel. Workers run concurrently while
        // main is still on this thread; we busy-spin (V2-isolated) before assembling
        // the snapshot. Visibility reflects last frame's ImGui state via
        // Panel::BeginWindow — first frame after a panel becomes visible runs OnDraw
        // against an empty snapshot fragment, then re-gathers next frame.
        JobSystem::Counter gatherCounter;
        const bool launcherOpen = ProjectLauncher::IsVisible();
        if (!launcherOpen)
        {
            for (auto& panel : s_Panels)
            {
                if (!panel->IsVisible() || panel->m_Crashed) continue;
                JobSystem::Execute(GatherJobThunk, panel.get(), &gatherCounter,
                                   "Editor.Gather", JobSystem::Priority::Low);
            }
            JobSystem::WaitForCounter(&gatherCounter);
        }

        // ── Snapshot assembly (single-threaded post-wait, no contention) ────────
        EditorSnapshot snapshot;
        for (auto& panel : s_Panels)
        {
            if (panel->IsVisible() && panel->m_SnapshotFragment)
                snapshot.m_Fragments[panel->m_FragmentType] = panel->m_SnapshotFragment;
        }

        // Create dockspace
        static bool dockspaceOpen = true;
        static ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_None;

        // Fullscreen parent window for dockspace
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags hostWindowFlags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_MenuBar;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGui::Begin("DockSpaceHost", &dockspaceOpen, hostWindowFlags);
        ImGui::PopStyleVar(3);

        // Menu bar
        DrawMenuBar();

        // Create dockspace
        ImGuiID dockspaceID = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspaceID, ImVec2(0.0f, 0.0f), dockspaceFlags);

        // Keyboard shortcuts
        ProcessShortcuts();

        // Dirty bumps now arrive via HierarchyChangedSignal subscription
        // installed in Init (sub-task F of v2.9.1 editor-signal-bus).

        // Update window title
        UpdateWindowTitle();

        // Show launcher or normal editor
        if (ProjectLauncher::IsVisible())
        {
            ProjectLauncher::Render();
        }
        else
        {
            for (auto& panel : s_Panels)
            {
                if (panel->m_Crashed) continue;
                panel->OnDraw(snapshot);
            }
        }

        // Check if a model import completed with unresolved textures
        {
            static fs::path s_LastReportedModel;
            const ImportReport& report = ModelImporter::GetLastImportReport();
            if (report.HasUnresolved() && report.ModelPath != s_LastReportedModel) {
                s_LastReportedModel = report.ModelPath;
                TextureRemapDialog::Open(report);
            }
        }

        // Draw texture remap modal (no-op when closed)
        TextureRemapDialog::Draw();

        ImGui::End();
    }

    bool Editor::WantCaptureMouse()
    {
        if (!s_Context) return false;
        return ImGui::GetIO().WantCaptureMouse;
    }

    bool Editor::WantCaptureKeyboard()
    {
        if (!s_Context) return false;
        return ImGui::GetIO().WantCaptureKeyboard;
    }

    void Editor::AddPanel(Panel* panel)
    {
        LH_CORE_ASSERT(panel, "Tried to add null panel");
        s_PanelRegistry[std::type_index(typeid(*panel))] = panel;
        s_Panels.emplace_back(panel);
    }

    void Editor::LoadStyle(const std::string& nameOrPath)
    {
        s_PendingStyle = nameOrPath;
    }

    void Editor::SetRandomStyle()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;
    
        // Seed with current time
        static std::mt19937 rng(std::chrono::system_clock::now().time_since_epoch().count());
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        std::uniform_real_distribution<float> colorDist(0.2f, 0.8f);
        std::uniform_real_distribution<float> propDist(0.5f, 3.0f);

        // Generate random base hue
        float hue = dist(rng);
        ImVec4 baseColor = ImColor::HSV(hue, 0.7f, 0.7f);
    
        // Random style properties
        style.WindowRounding = propDist(rng);
        style.ChildRounding = propDist(rng);
        style.FrameRounding = propDist(rng);
        style.GrabRounding = propDist(rng);
        style.PopupRounding = propDist(rng);
        style.ScrollbarRounding = propDist(rng);
    
        style.WindowBorderSize = dist(rng) > 0.5f ? 1.0f : 0.0f;
        style.FrameBorderSize = dist(rng) > 0.3f ? 1.0f : 0.0f;
    
        // Random color scheme
        colors[ImGuiCol_Text]             = ImVec4(dist(rng), dist(rng), dist(rng), 1.00f);
        colors[ImGuiCol_WindowBg]         = ImColor::HSV(hue, 0.2f, 0.2f);
        colors[ImGuiCol_ChildBg]          = ImColor::HSV(hue, 0.25f, 0.25f);
        colors[ImGuiCol_PopupBg]          = ImColor::HSV(hue, 0.2f, 0.3f);
        colors[ImGuiCol_Border]           = ImColor::HSV(hue, 0.4f, 0.6f);
        colors[ImGuiCol_FrameBg]          = ImColor::HSV(hue, 0.3f, 0.3f);
        colors[ImGuiCol_FrameBgHovered]   = ImColor::HSV(hue, 0.4f, 0.4f);
        colors[ImGuiCol_FrameBgActive]    = ImColor::HSV(hue, 0.5f, 0.5f);
        colors[ImGuiCol_TitleBg]          = ImColor::HSV(hue, 0.6f, 0.3f);
        colors[ImGuiCol_TitleBgActive]    = ImColor::HSV(hue, 0.7f, 0.4f);
        colors[ImGuiCol_CheckMark]        = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        colors[ImGuiCol_SliderGrab]       = ImColor::HSV(hue, 0.8f, 0.8f);
        colors[ImGuiCol_SliderGrabActive] = ImColor::HSV(hue, 0.9f, 0.9f);
        colors[ImGuiCol_Button]           = ImColor::HSV(hue, 0.6f, 0.4f);
        colors[ImGuiCol_ButtonHovered]    = ImColor::HSV(hue, 0.7f, 0.5f);
        colors[ImGuiCol_ButtonActive]     = ImColor::HSV(hue, 0.8f, 0.6f);
        colors[ImGuiCol_Header]           = ImColor::HSV(hue, 0.5f, 0.3f);
        colors[ImGuiCol_HeaderHovered]    = ImColor::HSV(hue, 0.6f, 0.4f);
        colors[ImGuiCol_HeaderActive]     = ImColor::HSV(hue, 0.7f, 0.5f);
    
        // Random spacing/padding
        style.WindowPadding = ImVec2(propDist(rng), propDist(rng));
        style.FramePadding = ImVec2(propDist(rng), propDist(rng));
        style.ItemSpacing = ImVec2(propDist(rng), propDist(rng));
        style.ItemInnerSpacing = ImVec2(propDist(rng), propDist(rng));

        // Random window transparency
        style.Alpha = 0.8f + dist(rng) * 0.2f;

        LH_CORE_INFO("Applied random style - Hue: {0}, WindowRounding: {1}",
                    hue, style.WindowRounding);
    }

    // ── Scene Management ──

    void Editor::SetActiveScene(std::shared_ptr<Scene> scene)
    {
        s_ActiveScene = scene;
        CommandHistory::Clear();

        // Update Systems raw pointer so TransformSystem/RenderingSystem use the correct scene
        SystemRegistry::SetScene(scene.get());

        // Update panels that hold scene context
        if (auto* hp = GetPanel<HierarchyPanel>())
            hp->SetContext(scene);
        if (auto* sp = GetPanel<ScenePanel>())
            sp->SetContext(scene);

        // Load last opened scene (on first call from App::Init, s_ActiveScene is now valid)
        if (!s_Settings.lastSceneUUID.empty())
        {
            UUID sceneUUID = UUID::FromString(s_Settings.lastSceneUUID);
            if (sceneUUID.IsValid() && AssetDatabase::Exists(sceneUUID))
            {
                const auto& meta = AssetDatabase::GetMetadata(sceneUUID);
                if (!meta.Path.empty() && fs::exists(meta.Path))
                    OpenScene(meta.Path);
            }
            s_Settings.lastSceneUUID.clear(); // One-shot: don't re-trigger on subsequent SetActiveScene calls
        }
    }

    void Editor::NewScene()
    {
        if (!s_ActiveScene) return;

        // Clear all entities
        s_ActiveScene->Clear();
        CommandHistory::Clear();

        s_ScenePath.clear();
        s_IsDirty = false;

        LH_CORE_INFO("New scene created");
    }

    void Editor::OpenScene()
    {
        auto path = FileDialog::OpenFile("Luth Scene (*.luth)\0*.luth\0All Files (*.*)\0*.*\0");
        if (!path.has_value()) return;
        OpenScene(path.value());
    }

    void Editor::OpenScene(const fs::path& path)
    {
        if (!s_ActiveScene) return;

        if (SceneSerializer::Load(*s_ActiveScene, path)) {
            CommandHistory::Clear();
            s_ScenePath = path;
            s_IsDirty = false;
            s_Settings.lastSceneUUID = AssetDatabase::GetUUID(path).ToString();

            // Eagerly kick off loading for all assets referenced by the scene
            auto view = s_ActiveScene->GetAllEntitiesWith<Component::MeshRenderer>();
            for (auto entity : view) {
                const auto& mr = view.get<Component::MeshRenderer>(entity);
                if (mr.ModelUUID.IsValid())
                    AssetManager::LoadAsync(mr.ModelUUID);
                if (mr.MaterialUUID.IsValid()) {
                    auto mat = std::static_pointer_cast<Material>(
                        AssetManager::LoadImmediate(mr.MaterialUUID));
                    if (mat) {
                        for (const auto& mapInfo : mat->GetTextures())
                            if (mapInfo.Uuid.IsValid())
                                AssetManager::LoadAsync(mapInfo.Uuid);
                    }
                }
            }
        }
    }

    void Editor::SaveScene()
    {
        if (!s_ActiveScene) return;

        if (s_ScenePath.empty()) {
            SaveSceneAs();
            return;
        }

        if (SceneSerializer::Save(*s_ActiveScene, s_ScenePath)) {
            s_IsDirty = false;

            // Ensure .meta file exists
            fs::path metaPath = s_ScenePath;
            metaPath += ".meta";
            if (!fs::exists(metaPath)) {
                MetaFile::Create(s_ScenePath, AssetType::Scene);
            }
        }
    }

    void Editor::SaveSceneAs()
    {
        auto path = FileDialog::SaveFile("Luth Scene (*.luth)\0*.luth\0All Files (*.*)\0*.*\0");
        if (!path.has_value()) return;

        s_ScenePath = path.value();
        SaveScene();
    }

    void Editor::MarkDirty()
    {
        s_IsDirty = true;
    }

    void Editor::ResetDirtyState(bool dirty)
    {
        s_IsDirty = dirty;
    }

    // ── Settings & Layout ──

    void Editor::LoadSettings()
    {
        s_SettingsPath = "editor_settings.json";
        s_Settings = EditorSettings::Load(s_SettingsPath);
    }

    void Editor::SaveSettings()
    {
        if (!s_SettingsPath.empty())
            EditorSettings::Save(s_Settings, s_SettingsPath);
    }

    void Editor::SaveLayout(const std::string& name)
    {
        namespace fs = std::filesystem;
        fs::path layoutDir = "layouts";
        if (!fs::exists(layoutDir))
            fs::create_directories(layoutDir);

        size_t size = 0;
        const char* iniData = ImGui::SaveIniSettingsToMemory(&size);

        fs::path layoutPath = layoutDir / (name + ".ini");
        std::ofstream file(layoutPath);
        if (file.is_open()) {
            file.write(iniData, size);
            s_Settings.activeLayout = name;
            LH_CORE_INFO("Saved layout '{}' to '{}'", name, layoutPath.string());
        }
    }

    void Editor::LoadLayout(const std::string& name)
    {
        namespace fs = std::filesystem;
        fs::path layoutPath = fs::path("layouts") / (name + ".ini");

        if (!fs::exists(layoutPath)) {
            LH_CORE_WARN("Layout '{}' not found", name);
            return;
        }

        std::ifstream file(layoutPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return;

        auto size = file.tellg();
        file.seekg(0);
        std::string data(size, '\0');
        file.read(data.data(), size);

        ImGui::LoadIniSettingsFromMemory(data.c_str(), data.size());
        s_Settings.activeLayout = name;
        LH_CORE_INFO("Loaded layout '{}'", name);
    }

    std::vector<std::string> Editor::GetLayoutNames()
    {
        namespace fs = std::filesystem;
        std::vector<std::string> names;
        fs::path layoutDir = "layouts";

        if (!fs::exists(layoutDir))
            return names;

        for (const auto& entry : fs::directory_iterator(layoutDir)) {
            if (entry.path().extension() == ".ini")
                names.push_back(entry.path().stem().string());
        }

        std::sort(names.begin(), names.end());
        return names;
    }

    void Editor::ProcessShortcuts()
    {
        bool ctrl = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
        bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);

        if (ctrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                if (shift)
                    CommandHistory::Redo();
                else
                    CommandHistory::Undo();
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
                CommandHistory::Redo();
            else if (ImGui::IsKeyPressed(ImGuiKey_N, false))
                NewScene();
            else if (ImGui::IsKeyPressed(ImGuiKey_O, false))
                OpenScene();
            else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                if (shift)
                    SaveSceneAs();
                else
                    SaveScene();
            }
        }
    }

    void Editor::DrawMenuBar()
    {
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open Project..."))
                {
                    auto path = FileDialog::OpenFile("Luth Project (*.luthproj)\0*.luthproj\0All Files (*.*)\0*.*\0");
                    if (path.has_value())
                    {
                        ProjectLauncher::AddRecent(path.value().stem().string(), path.value());
                        ProjectLauncher::SetPendingProject(path.value());
                    }
                }
                if (ImGui::MenuItem(ICON_FA_CUBE "  Project Launcher..."))
                    ShowProjectLauncher();

                ImGui::Separator();

                if (ImGui::MenuItem("New Scene", "Ctrl+N"))
                    NewScene();
                if (ImGui::MenuItem("Open Scene", "Ctrl+O"))
                    OpenScene();
                ImGui::Separator();
                if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
                    SaveScene();
                if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S"))
                    SaveSceneAs();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, CommandHistory::CanUndo()))
                    CommandHistory::Undo();
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, CommandHistory::CanRedo()))
                    CommandHistory::Redo();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Assets")) {
                const ImportReport& report = ModelImporter::GetLastImportReport();
                bool hasUnresolved = report.HasUnresolved();

                if (!hasUnresolved) ImGui::BeginDisabled();
                if (ImGui::MenuItem("Resolve Missing Textures..."))
                    s_ShowTextureRemapDialog = true;
                if (!hasUnresolved) ImGui::EndDisabled();

                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("View")) {
                // Style submenu — deferred to next BeginFrame (font atlas can't change mid-frame)
                if (ImGui::BeginMenu("Style")) {
                    auto deferBuiltin = [](const char* name) {
                        bool active = s_Settings.activeStylePath.empty()
                                   && s_Settings.activeStyle == name;
                        if (ImGui::MenuItem(name, nullptr, active))
                            s_PendingStyle = name;
                    };
                    deferBuiltin("Custom");
                    deferBuiltin("Bubblegum");
                    deferBuiltin("Matrix");
                    deferBuiltin("Rider");

                    ImGui::Separator();

                    if (ImGui::MenuItem("Load From File...")) {
                        auto picked = FileDialog::OpenFile("Luth Style (*.json)\0*.json\0All Files (*.*)\0*.*\0");
                        if (picked.has_value())
                            s_PendingStyle = picked.value().string();
                    }
                    if (ImGui::MenuItem("Save Current As...", nullptr, false, s_CurrentStyle.has_value())) {
                        auto picked = FileDialog::SaveFile("Luth Style (*.json)\0*.json\0All Files (*.*)\0*.*\0");
                        if (picked.has_value()) {
                            fs::path out = picked.value();
                            if (out.extension() != ".json") out += ".json";
                            EditorStyle::SaveToFile(s_CurrentStyle->Preset, s_CurrentStyle->Font, out);
                        }
                    }
                    ImGui::EndMenu();
                }

                ImGui::Separator();

                // Layout submenu
                if (ImGui::BeginMenu("Layouts")) {
                    auto names = GetLayoutNames();
                    for (const auto& name : names) {
                        bool isActive = (s_Settings.activeLayout == name);
                        if (ImGui::MenuItem(name.c_str(), nullptr, isActive))
                            LoadLayout(name);
                    }

                    if (!names.empty())
                        ImGui::Separator();

                    if (ImGui::MenuItem("Save Layout..."))
                        s_ShowSaveLayoutPopup = true;

                    ImGui::EndMenu();
                }

                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }

        // Texture remap popup — deferred open from menu
        if (s_ShowTextureRemapDialog) {
            const ImportReport& report = ModelImporter::GetLastImportReport();
            if (report.HasUnresolved())
                TextureRemapDialog::Open(report);
            s_ShowTextureRemapDialog = false;
        }

        // Save Layout popup — rendered outside menu scope so ImGui can track it
        if (s_ShowSaveLayoutPopup) {
            ImGui::OpenPopup("Save Layout");
            s_ShowSaveLayoutPopup = false;
        }

        if (ImGui::BeginPopupModal("Save Layout", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char layoutName[128] = "";
            ImGui::Text("Layout Name:");
            ImGui::SetNextItemWidth(250.0f);
            ImGui::InputText("##LayoutName", layoutName, sizeof(layoutName));

            ImGui::Spacing();
            if (ImGui::Button("Save", ImVec2(120, 0)) && layoutName[0] != '\0') {
                SaveLayout(layoutName);
                layoutName[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                layoutName[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void Editor::UpdateWindowTitle()
    {
        if (!s_Window) return;

        std::string title = Luth::GetFullTitleString();
        if (!s_ScenePath.empty()) {
            title += " - " + s_ScenePath.filename().string();
        }
        else {
            title += " - Untitled";
        }
        if (s_IsDirty) {
            title += "*";
        }

        GLFWwindow* win = (GLFWwindow*)s_Window->GetNativeWindow();
        if (win) {
            glfwSetWindowTitle(win, title.c_str());
        }
    }

    void Editor::ShowProjectLauncher()
    {
        ProjectLauncher::Show();
    }

    void Editor::OnProjectChanged()
    {
        // Reload editor settings from the new project directory
        LoadSettings();

        // Clear scene state
        s_ScenePath.clear();
        s_IsDirty = false;

        // Refresh ProjectPanel to scan new assets directory
        if (auto* pp = GetPanel<ProjectPanel>())
        {
            pp->Refresh();
            pp->SetThumbnailSize(s_Settings.thumbnailSize);
        }

        // Reset hierarchy selection
        if (auto* hp = GetPanel<HierarchyPanel>())
            hp->SetContext(s_ActiveScene);

        // Reload the skybox now that the project's asset paths are live.
        // RenderingSystem::ctor runs before any project is loaded, so its IBL
        // init falls back to engine-assets (which don't ship an HDR). Once the
        // project root is set, re-resolve the settings path and reload.
        if (auto rs = SystemRegistry::GetSystem<RenderingSystem>();
            rs && !s_Settings.skyboxPath.empty())
        {
            fs::path skyboxAbsPath = fs::path(s_Settings.skyboxPath).is_absolute()
                ? fs::path(s_Settings.skyboxPath)
                : FileSystem::ResolveAsset(s_Settings.skyboxPath);
            if (fs::exists(skyboxAbsPath))
                rs->ReloadSkybox(skyboxAbsPath);
        }

        // Broadcast project switch to panels. Path stays empty when called from
        // shutdown / unload (no project loaded yet); subscribers should treat
        // empty path as "project unloaded" rather than "default project."
        const std::string projPath = FileSystem::ProjectPath().string();
        const std::string projName = FileSystem::ProjectPath().filename().string();
        EventBus::Enqueue<ProjectChangedSignal>(BusType::MainThread, projPath, projName);

        LH_CORE_INFO("Editor: Project changed, panels refreshed");
    }
}
