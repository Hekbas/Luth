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
#include "luthien/EditorSelection.h"
#include "luthien/commands/EntityCommands.h"
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
#include "luthien/panels/ConsolePanel.h"
#include "luthien/panels/MaterialGraphPanel.h"
#include "luthien/panels/EditorSettingsWindow.h"
#include "luthien/panels/TextureRemapDialog.h"
#include "luth/resources/importers/ModelImporter.h"
#include "luthien/widgets/Widgets.h"
#include "luthien/widgets/ThumbnailCache.h"
#include "luthien/widgets/ThumbnailPreviewScene.h"
#include "luthien/EditorAutoSave.h"
#include "luthien/EditorStyle.h"
#include "luthien/Workspace.h"
#include "luth/core/Version.h"
#include "luth/core/diagnostics/StackTrace.h"
#include "luthien/EditorSettings.h"
#include "luthien/widgets/Icons.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>

namespace Luth
{
    // Cached so Style > Save Current As can round-trip the live style.
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
        LH_LOG(Editor, info, "Initializing Luth Editor");

        // Settings load first so InitImGui can apply the persisted style, which populates io.Fonts
        // before the Vulkan font atlas is built.
        LoadSettings();
        InitImGui(window);
        ProjectLauncher::Init();
        InitPanels();
        ApplyPersistence();

        // Snapshot the live ImGui dock layout into layouts/Default.ini on first run so
        // Window > Reset Layout always has a fallback target. Deferred to end of first Render;
        // ImGui hasn't built dock state yet.
        s_NeedDefaultLayoutSave = !fs::exists("layouts/Default.ini");

        // Auto-apply the persisted active workspace at end of first Render. Panels exist by then;
        // ImGui dock state has built. Empty string skips (fresh user before any save).
        s_NeedActiveWorkspaceLoad = !s_Settings.activeLayout.empty();

        // Forward AssetDatabase file-watch flushes onto the EventBus as typed AssetChangedSignals so
        // panels (Project/Resource/Inspector/ThumbnailCache) react via subscriptions instead of polling.
        // The dirty-UUID list is all the AssetDatabase callback API exposes, so publish Modified for
        // everything; subscribers that need to distinguish import-vs-delete query
        // AssetDatabase::Exists(uuid) themselves.
        AssetDatabase::AddChangeCallback([]() {
            for (const UUID& uuid : AssetDatabase::GetDirtyAssets()) {
                EventBus::Enqueue<AssetChangedSignal>(BusType::MainThread,
                    AssetChangedSignal::Op::Modified, uuid);
            }
        });

        // Reactive dirty-marking driven by signals. Every EntityCommand publishes
        // HierarchyChangedSignal; this handler bumps the dirty flag exactly when user edits land.
        // Scene LOAD bypasses commands, so the dirty flag stays clean across open/close.
        EventBus::Subscribe<HierarchyChangedSignal>(BusType::MainThread,
            [](Event&) { Editor::MarkDirty(); });

        EditorAutoSave::Init();
        UI::ThumbnailCache::Init();
        UI::ThumbnailPreviewScene::Init();
    }

    void Editor::InitImGui(Window* window)
    {
        IMGUI_CHECKVERSION();
        s_Context = ImGui::CreateContext();
        LH_LOG(Editor, trace, " - Created ImGui context");

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        LH_LOG(Editor, trace, " - Enabled docking + multi-viewport");

        // Apply persisted style BEFORE CreateFontsTexture: LoadFonts populates io.Fonts.
        // Window-chrome colors belong here too since Matrix tints them.
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
            LH_LOG(Editor, trace, " - Initialized ImGui GLFW/Vulkan backend");
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
        AddPanel(new ConsolePanel());
        AddPanel(new MaterialGraphPanel());

        ComponentDrawers::RegisterComponentDrawers();

        for (auto& panel : s_Panels)
            panel->OnInit();
    }

    void Editor::ApplyPersistence()
    {
        // Hydrate per-panel visibility from the persisted map. Missing keys keep
        // Panel's default (true) so panels added in later versions appear by default.
        for (auto& panel : s_Panels)
        {
            auto it = s_Settings.panelOpen.find(panel->GetWindowID());
            if (it != s_Settings.panelOpen.end())
                panel->m_Open = it->second;
        }

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
        // Sync panel state into settings before saving.
        if (auto* sp = GetPanel<ScenePanel>()) {
            sp->GetEditorCamera().SyncToSettings(s_Settings);
            s_Settings.showControlsOverlay = sp->GetShowControlsOverlay();
        }
        if (auto* pp = GetPanel<ProjectPanel>())
            s_Settings.thumbnailSize = pp->GetThumbnailSize();
        s_Settings.lastSceneUUID = s_ScenePath.empty() ? "" : AssetDatabase::GetUUID(s_ScenePath).ToString();
        CaptureSceneView();

        SaveSettings();
        SaveActiveWorkspaceSidecar();

        UI::ThumbnailPreviewScene::Shutdown();
        UI::ThumbnailCache::Shutdown();
        EditorAutoSave::Shutdown();

        LH_LOG(Editor, trace, "Cleaning up {} panels", s_Panels.size());
        // Run OnShutdown before destroying so panels detach from engine subsystems (Log sinks,
        // EventBus subscriptions) while the rest of the editor is still alive. Without this hop,
        // a post-clear LH_CORE_* call would walk dangling sink pointers.
        for (auto& panel : s_Panels) panel->OnShutdown();
        ComponentDrawerRegistry::Shutdown();
        s_PanelRegistry.clear();
        s_Panels.clear();
        UI::ClearTextureCache();

        // Drain any pending fenced deletions while ImGui Vulkan is still alive. PushDeletion lambdas
        // (UI::GetTextureID stale-eviction, ThumbnailCache Invalidate/Drain) call
        // ImGui_ImplVulkan_RemoveTexture; if they fire later from App::Close ->
        // Renderer::FlushDeletionQueues, the ImGui backend has already torn down, null-deref crash on
        // the descriptor pool. invariant: must run before ImGui_ImplVulkan_Shutdown below.
        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan)
            VulkanContext::Get().FlushAllDeletionQueues();

        if (Renderer::GetBackend()->GetAPI() == RenderBackend::API::Vulkan) {
            // Clear GLFW callbacks that forward to ImGui BEFORE destroying the backend. These were
            // installed manually (install_callbacks=false), so ImGui_ImplGlfw_Shutdown won't remove
            // them. Without this, Win32 focus messages during Vulkan teardown dispatch into freed
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
        LH_LOG(Editor, info, "Editor system shutdown completed");
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

        // Reset the panel's scratch first: Reset() rewinds the bump pointer without freeing pages,
        // so the prior frame's m_SnapshotFragment is invalid from this line onward. Null it
        // explicitly so a thrown OnGather doesn't leave a dangling pointer for the snapshot-assembly
        // phase to read.
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
            LH_LOG(Editor, error, "Panel '{}' threw in OnGather: {}", panel->GetWindowID(), e.what());
            StackTrace::LogStackTrace(1, 32);
            panel->m_SnapshotFragment = nullptr;
            if (++panel->m_CrashStreak >= 3) panel->m_Crashed = true;
        }
        catch (...)
        {
            LH_LOG(Editor, error, "Panel '{}' threw non-std exception in OnGather", panel->GetWindowID());
            StackTrace::LogStackTrace(1, 32);
            panel->m_SnapshotFragment = nullptr;
            if (++panel->m_CrashStreak >= 3) panel->m_Crashed = true;
        }
    }

    void Editor::DrawPanelGuarded(Panel* panel, const EditorSnapshot& snapshot)
    {
        try
        {
            panel->OnDraw(snapshot);
        }
        catch (const std::exception& e)
        {
            LH_LOG(Editor, error, "Panel '{}' threw in OnDraw: {}", panel->GetWindowID(), e.what());
            StackTrace::LogStackTrace(1, 32);
            if (++panel->m_CrashStreak >= 3) panel->m_Crashed = true;
        }
        catch (...)
        {
            LH_LOG(Editor, error, "Panel '{}' threw non-std exception in OnDraw", panel->GetWindowID());
            StackTrace::LogStackTrace(1, 32);
            if (++panel->m_CrashStreak >= 3) panel->m_Crashed = true;
        }
    }

    void Editor::DrawCrashedPlaceholder(Panel* panel)
    {
        // Reuse the panel's window ID so docking persists; an unresponsive panel becomes a
        // clearly-marked stub that the user can revive after fixing.
        if (panel->BeginWindow(panel->GetWindowID()))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::PushFont(GetIconRegular());
            ImGui::TextUnformatted(ICON_WARNING);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextUnformatted("Panel crashed.");
            ImGui::PopStyleColor();
            ImGui::TextDisabled("See Console for the stack trace.");
            ImGui::Spacing();
            if (ImGui::Button("Reset"))
            {
                panel->m_Crashed     = false;
                panel->m_CrashStreak = 0;
            }
        }
        ImGui::End();
    }

    void Editor::Render()
    {
        LH_PROFILE_FUNCTION();

        // Skip rendering if context is null (Vulkan case)
        if (!s_Context) return;

        EditorAutoSave::Tick();
        UI::ThumbnailCache::Drain();

        // ---- Gather phase ----
        // Dispatch one gather job per visible panel. Workers run concurrently while main is still on
        // this thread; busy-spin (V2-isolated) before assembling the snapshot. Visibility reflects
        // last frame's ImGui state via Panel::BeginWindow: first frame after a panel becomes visible
        // runs OnDraw against an empty snapshot fragment, then re-gathers next frame.
        JobSystem::Counter gatherCounter;
        const bool launcherOpen = ProjectLauncher::IsVisible();
        if (!launcherOpen)
        {
            for (auto& panel : s_Panels)
            {
                if (!panel->m_Open || !panel->IsVisible() || panel->m_Crashed) continue;
                JobSystem::Execute(GatherJobThunk, panel.get(), &gatherCounter,
                                   "Editor.Gather", JobSystem::Priority::Low);
            }
            JobSystem::WaitForCounter(&gatherCounter);
        }

        // ---- Snapshot assembly (single-threaded post-wait, no contention) ----
        EditorSnapshot snapshot;
        for (auto& panel : s_Panels)
        {
            if (panel->m_Open && panel->IsVisible() && panel->m_SnapshotFragment)
                snapshot.m_Fragments[panel->m_FragmentType] = panel->m_SnapshotFragment;
        }

        static bool dockspaceOpen = true;
        static ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_None;

        // Fullscreen host window backing the dockspace.
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

        DrawMenuBar();

        ImGuiID dockspaceID = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspaceID, ImVec2(0.0f, 0.0f), dockspaceFlags);

        ProcessShortcuts();

        // Dirty bumps arrive via the HierarchyChangedSignal subscription installed in Init.

        UpdateWindowTitle();

        if (ProjectLauncher::IsVisible())
        {
            ProjectLauncher::Render();
        }
        else
        {
            for (auto& panel : s_Panels)
            {
                if (!panel->m_Open) continue;
                if (panel->m_Crashed) {
                    DrawCrashedPlaceholder(panel.get());
                    continue;
                }
                DrawPanelGuarded(panel.get(), snapshot);
            }
        }

        // Preferences window: drawn outside the dockspace's panel loop so it floats independently
        // and is not tied to any dock node.
        EditorSettingsWindow::Draw();

        // Check if a model import completed with unresolved or reduced-fidelity textures
        {
            static fs::path s_LastReportedModel;
            const ImportReport& report = ModelImporter::GetLastImportReport();
            if (report.ModelPath != s_LastReportedModel && (report.HasUnresolved() || report.HasDegraded())) {
                s_LastReportedModel = report.ModelPath;
                if (report.HasDegraded())
                    LH_LOG(Editor, warn, "Import: {0} texture(s) routed at reduced fidelity (see warnings above)",
                                 report.Degraded.size());
                if (report.HasUnresolved())
                    TextureRemapDialog::Open(report);
            }
        }

        // Draw texture remap modal (no-op when closed)
        TextureRemapDialog::Draw();

        // Crash-recovery prompt (no-op when nothing pending)
        EditorAutoSave::DrawRecoveryModal();

        // First-run default layout snapshot (see s_NeedDefaultLayoutSave decl).
        if (s_NeedDefaultLayoutSave)
        {
            fs::path layoutDir = "layouts";
            if (!fs::exists(layoutDir)) fs::create_directories(layoutDir);
            size_t size = 0;
            const char* iniData = ImGui::SaveIniSettingsToMemory(&size);
            std::ofstream f(layoutDir / "Default.ini");
            if (f.is_open()) {
                f.write(iniData, size);
                LH_LOG(Editor, info, "Saved first-run default layout to layouts/Default.ini");
            }
            s_NeedDefaultLayoutSave = false;
        }

        // Persisted active workspace: runs after the snapshot so a built-in Default overrides
        // whatever ImGui's first-frame state was. One-frame flash of the un-docked default state is
        // acceptable on fresh installs.
        if (s_NeedActiveWorkspaceLoad)
        {
            LoadWorkspace(s_Settings.activeLayout);
            s_NeedActiveWorkspaceLoad = false;
        }

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
    
        static std::mt19937 rng(std::chrono::system_clock::now().time_since_epoch().count());
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        std::uniform_real_distribution<float> colorDist(0.2f, 0.8f);
        std::uniform_real_distribution<float> propDist(0.5f, 3.0f);

        float hue = dist(rng);
        ImVec4 baseColor = ImColor::HSV(hue, 0.7f, 0.7f);
    
        style.WindowRounding = propDist(rng);
        style.ChildRounding = propDist(rng);
        style.FrameRounding = propDist(rng);
        style.GrabRounding = propDist(rng);
        style.PopupRounding = propDist(rng);
        style.ScrollbarRounding = propDist(rng);
    
        style.WindowBorderSize = dist(rng) > 0.5f ? 1.0f : 0.0f;
        style.FrameBorderSize = dist(rng) > 0.3f ? 1.0f : 0.0f;
    
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
    
        style.WindowPadding = ImVec2(propDist(rng), propDist(rng));
        style.FramePadding = ImVec2(propDist(rng), propDist(rng));
        style.ItemSpacing = ImVec2(propDist(rng), propDist(rng));
        style.ItemInnerSpacing = ImVec2(propDist(rng), propDist(rng));

        style.Alpha = 0.8f + dist(rng) * 0.2f;

        LH_LOG(Editor, info, "Applied random style - Hue: {0}, WindowRounding: {1}",
                    hue, style.WindowRounding);
    }

    // ---- Scene Management ----

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

        // The auto-load-from-lastSceneUUID flow used to live here, but SetActiveScene fires from
        // App::App() before LoadProject populates AssetDatabase, so Exists() always returned false.
        // Auto-load now runs in OnProjectChanged, after the database is live.
    }

    void Editor::NewScene()
    {
        if (!s_ActiveScene) return;

        // Persist the outgoing scene's camera before clearing.
        CaptureSceneView();

        s_ActiveScene->Clear();
        CommandHistory::Clear();

        // Seed the fresh scene with a sun + camera so it doesn't open onto a black, unlit viewport
        // (mirrors the hierarchy "Create > Directional Light / Camera" defaults). Created directly,
        // not via commands, since NewScene wipes the undo history anyway.
        Entity sun = s_ActiveScene->CreateEntity("Directional Light");
        sun.AddComponent<Component::DirectionalLight>();
        {
            auto& t = sun.GetComponent<Component::Transform>();
            t.Rotation = Vec3(-45.0f, 0.0f, 0.0f);
            t.IsDirty  = true;
        }
        Entity cam = s_ActiveScene->CreateEntity("Camera");
        cam.AddComponent<Component::Camera>();
        {
            auto& t = cam.GetComponent<Component::Transform>();
            t.Position = Vec3(0.0f, 1.0f, 5.0f);
            t.IsDirty  = true;
        }

        s_ScenePath.clear();
        s_IsDirty = false;

        LH_LOG(Editor, info, "New scene created");
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

        // Persist the outgoing scene's camera before swapping scenes.
        CaptureSceneView();

        if (SceneSerializer::Load(*s_ActiveScene, path)) {
            CommandHistory::Clear();
            s_ScenePath = path;
            s_IsDirty = false;
            std::string sceneUUID = AssetDatabase::GetUUID(path).ToString();
            s_Settings.lastSceneUUID = sceneUUID;

            // Restore the editor camera to where this scene was last framed.
            RestoreSceneView(sceneUUID);

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

            // Surface a fresher autosave after a crash mid-edit on this scene. Covers auto-load
            // (OnProjectChanged) AND manual File > Open paths.
            EditorAutoSave::ScanForRecovery(s_ScenePath);
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

            fs::path metaPath = s_ScenePath;
            metaPath += ".meta";
            if (!fs::exists(metaPath)) {
                MetaFile::Create(s_ScenePath, AssetType::Scene);
            }

            // Persist the current scene-view camera alongside the save.
            CaptureSceneView();
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

    // ---- Settings & Layout ----

    void Editor::LoadSettings()
    {
        s_SettingsPath = "editor_settings.json";
        s_Settings = EditorSettings::Load(s_SettingsPath);
    }

    void Editor::SaveSettings()
    {
        // Mirror live panel visibility into the settings map. Skipping this would lose Window-menu
        // toggles between the toggle and the next save trigger.
        for (auto& panel : s_Panels)
            s_Settings.panelOpen[panel->GetWindowID()] = panel->m_Open;

        if (!s_SettingsPath.empty())
            EditorSettings::Save(s_Settings, s_SettingsPath);
    }

    std::filesystem::path Editor::SceneViewsPath()
    {
        return FileSystem::ProjectPath() / ".luth" / "scene_views.json";
    }

    void Editor::CaptureSceneView()
    {
        // Unsaved scenes have no path, hence no UUID to key the pose against.
        if (s_ScenePath.empty()) return;

        UUID uuid = AssetDatabase::GetUUID(s_ScenePath);
        if (!uuid.IsValid()) return;

        auto* sp = GetPanel<ScenePanel>();
        if (!sp) return;

        s_SceneViews.Set(uuid.ToString(), sp->GetEditorCamera().CapturePose());
        s_SceneViews.Save(SceneViewsPath());   // write-through: tiny file, crash-safe
    }

    void Editor::RestoreSceneView(const std::string& sceneUUID)
    {
        auto pose = s_SceneViews.Get(sceneUUID);
        if (!pose) return;

        if (auto* sp = GetPanel<ScenePanel>())
            sp->GetEditorCamera().ApplyPose(*pose);
    }

    namespace
    {
        // Built-ins live alongside other engine assets; user copies under cwd-relative
        // runtime/layouts/ (existing first-run snapshot path, no migration).
        std::filesystem::path BuiltinDir() { return FileSystem::EngineAssetsPath("workspaces"); }
        std::filesystem::path UserDir()    { return std::filesystem::path("layouts"); }

        std::filesystem::path BuiltinIni (const std::string& n) { return BuiltinDir() / (n + ".ini"); }
        std::filesystem::path BuiltinJson(const std::string& n) { return BuiltinDir() / (n + ".workspace.json"); }
        std::filesystem::path UserIni    (const std::string& n) { return UserDir()    / (n + ".ini"); }
        std::filesystem::path UserJson   (const std::string& n) { return UserDir()    / (n + ".workspace.json"); }
    }

    bool Editor::LoadWorkspace(const std::string& name)
    {
        namespace fs = std::filesystem;

        // Persist outgoing user workspace's panel visibility before switching so mid-session toggles
        // aren't lost. Built-in outgoing is a no-op.
        if (!s_Settings.activeLayout.empty() && s_Settings.activeLayout != name)
            SaveActiveWorkspaceSidecar();

        const fs::path bIni  = BuiltinIni(name);
        const fs::path bJson = BuiltinJson(name);
        const fs::path uIni  = UserIni(name);
        const fs::path uJson = UserJson(name);

        const fs::path iniPath  = fs::exists(bIni)  ? bIni  : uIni;
        const fs::path jsonPath = fs::exists(bJson) ? bJson : uJson;

        if (!fs::exists(iniPath)) {
            LH_LOG(Editor, warn, "Workspace '{}' not found", name);
            return false;
        }

        // Sidecar may be absent on legacy .ini-only workspaces: Workspace::LoadJson returns false in
        // that case and leaves panelOpen unchanged so the in-memory visibility set isn't clobbered.
        if (Workspace::LoadJson(jsonPath, s_Settings.panelOpen))
            ApplyPersistence();

        std::ifstream f(iniPath, std::ios::binary | std::ios::ate);
        if (!f.is_open()) {
            LH_LOG(Editor, error, "Failed to open workspace ini '{}'", iniPath.string());
            return false;
        }
        auto size = f.tellg();
        f.seekg(0);
        std::string data(size, '\0');
        f.read(data.data(), size);
        ImGui::LoadIniSettingsFromMemory(data.c_str(), data.size());

        s_Settings.activeLayout = name;

        const bool isBuiltin = Workspace::IsBuiltinPath(iniPath);
        EventBus::Enqueue<WorkspaceChangedSignal>(BusType::MainThread, name, isBuiltin);

        LH_LOG(Editor, info, "Loaded workspace '{}' from '{}'", name, iniPath.string());
        return true;
    }

    bool Editor::SaveWorkspaceAs(const std::string& name)
    {
        namespace fs = std::filesystem;

        if (fs::exists(BuiltinIni(name))) {
            LH_LOG(Editor, warn, "Cannot overwrite built-in workspace '{}' — pick a different name", name);
            return false;
        }

        const fs::path iniPath  = UserIni(name);
        const fs::path jsonPath = UserJson(name);
        if (!fs::exists(UserDir())) fs::create_directories(UserDir());

        // Snapshot live panel visibility at call time; s_Settings.panelOpen may be stale relative to
        // current m_Open state.
        std::unordered_map<std::string, bool> snap;
        for (auto& panel : s_Panels)
            snap[panel->GetWindowID()] = panel->m_Open;

        size_t size = 0;
        const char* iniData = ImGui::SaveIniSettingsToMemory(&size);
        std::ofstream f(iniPath);
        if (!f.is_open()) {
            LH_LOG(Editor, error, "Failed to write workspace ini '{}'", iniPath.string());
            return false;
        }
        f.write(iniData, size);
        f.close();

        if (!Workspace::SaveJson(jsonPath, snap))
            return false;

        s_Settings.panelOpen    = snap;
        s_Settings.activeLayout = name;
        LH_LOG(Editor, info, "Saved workspace '{}' to '{}'", name, iniPath.string());
        return true;
    }

    bool Editor::RenameWorkspace(const std::string& oldName, const std::string& newName)
    {
        namespace fs = std::filesystem;
        if (oldName == newName) return true;

        if (fs::exists(BuiltinIni(oldName))) {
            LH_LOG(Editor, warn, "Cannot rename built-in workspace '{}'", oldName);
            return false;
        }
        if (fs::exists(UserIni(newName)) || fs::exists(BuiltinIni(newName))) {
            LH_LOG(Editor, warn, "Cannot rename workspace '{}' to '{}': target exists", oldName, newName);
            return false;
        }

        std::error_code ec;
        if (fs::exists(UserIni(oldName)))  fs::rename(UserIni(oldName),  UserIni(newName),  ec);
        if (fs::exists(UserJson(oldName))) fs::rename(UserJson(oldName), UserJson(newName), ec);

        if (s_Settings.activeLayout == oldName)
            s_Settings.activeLayout = newName;

        LH_LOG(Editor, info, "Renamed workspace '{}' -> '{}'", oldName, newName);
        return true;
    }

    bool Editor::DeleteWorkspace(const std::string& name)
    {
        namespace fs = std::filesystem;

        if (fs::exists(BuiltinIni(name))) {
            LH_LOG(Editor, warn, "Cannot delete built-in workspace '{}'", name);
            return false;
        }

        std::error_code ec;
        bool removed = false;
        if (fs::exists(UserIni(name)))  { fs::remove(UserIni(name),  ec); removed = true; }
        if (fs::exists(UserJson(name))) { fs::remove(UserJson(name), ec); removed = true; }

        if (!removed) return false;

        if (s_Settings.activeLayout == name) {
            s_Settings.activeLayout = "Default";
            LoadWorkspace("Default");
        }
        LH_LOG(Editor, info, "Deleted workspace '{}'", name);
        return true;
    }

    bool Editor::ResetWorkspaceToBuiltin()
    {
        // LoadWorkspace prefers built-in path over user copy, so reloading the active name snaps
        // back to the shipped baseline (or last-saved if none).
        return LoadWorkspace(s_Settings.activeLayout);
    }

    std::vector<WorkspaceInfo> Editor::GetWorkspaces()
    {
        namespace fs = std::filesystem;
        std::vector<WorkspaceInfo> out;
        std::unordered_set<std::string> seen;

        if (fs::exists(BuiltinDir())) {
            for (const auto& e : fs::directory_iterator(BuiltinDir())) {
                if (e.path().extension() != ".ini") continue;
                const std::string name = e.path().stem().string();
                out.push_back({ name, true });
                seen.insert(name);
            }
        }

        if (fs::exists(UserDir())) {
            for (const auto& e : fs::directory_iterator(UserDir())) {
                if (e.path().extension() != ".ini") continue;
                const std::string name = e.path().stem().string();
                // Built-in shadows silently: SaveWorkspaceAs refuses colliding names, so the only way
                // a user copy collides is the first-run Default snapshot (intentional fallback, not
                // user error). Warning here would spam the log every frame from the
                // Window > Workspaces menu.
                if (seen.count(name)) continue;
                out.push_back({ name, false });
            }
        }

        std::sort(out.begin(), out.end(), [](const WorkspaceInfo& a, const WorkspaceInfo& b) {
            if (a.builtin != b.builtin) return a.builtin > b.builtin;   // built-ins first
            return a.name < b.name;
        });
        return out;
    }

    void Editor::SaveActiveWorkspaceSidecar()
    {
        namespace fs = std::filesystem;
        if (s_Settings.activeLayout.empty()) return;

        // Built-in workspaces are read-only: visibility tweaks made while a built-in is active
        // persist in-session only. User must "Save Current As..." to fork.
        if (fs::exists(BuiltinIni(s_Settings.activeLayout))) return;

        if (!fs::exists(UserDir())) fs::create_directories(UserDir());

        std::unordered_map<std::string, bool> snap;
        for (auto& panel : s_Panels)
            snap[panel->GetWindowID()] = panel->m_Open;

        Workspace::SaveJson(UserJson(s_Settings.activeLayout), snap);
    }

    void Editor::DeleteSelectedEntities()
    {
        const auto& sel = EditorSelection::GetSelectedEntities();
        if (sel.empty() || !s_ActiveScene) return;

        auto cmd = std::make_unique<EntityDestroyMultipleCommand>(s_ActiveScene.get(), sel);
        if (!cmd->IsEmpty())
            CommandHistory::Execute(std::move(cmd));
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
            else if (ImGui::IsKeyPressed(ImGuiKey_D, false)) {
                Entity sel = EditorSelection::GetSelectedEntity();
                if (sel && sel.IsValid())
                    CommandHistory::Execute(std::make_unique<EntityDuplicateCommand>(sel.GetScene(), sel));
            }
        }

        // Delete fires when an entity-editing context holds input: Hierarchy focused, or the Scene
        // viewport focused/hovered. Centralized here (not per-panel) so the two can't double-fire,
        // gated on !WantTextInput so it doesn't delete while renaming/typing. Runs before panels
        // draw, so no scene-tree iteration is active and the delete is immediate.
        if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            auto* hier  = GetPanel<HierarchyPanel>();
            auto* scene = GetPanel<ScenePanel>();
            const bool hierActive  = hier && hier->IsFocused();
            const bool sceneActive = scene && (scene->IsViewportFocused() || scene->IsViewportHovered());
            if (hierActive || sceneActive)
                DeleteSelectedEntities();
        }
    }

    void Editor::DrawMenuBar()
    {
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem(ICON_FOLDER_OPEN "  Open Project..."))
                {
                    auto path = FileDialog::OpenFile("Luth Project (*.luthproj)\0*.luthproj\0All Files (*.*)\0*.*\0");
                    if (path.has_value())
                    {
                        ProjectLauncher::AddRecent(path.value().stem().string(), path.value());
                        ProjectLauncher::SetPendingProject(path.value());
                    }
                }
                if (ImGui::MenuItem(ICON_CUBE "  Project Launcher..."))
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
                ImGui::Separator();
                if (ImGui::MenuItem("Autosave Now"))
                    EditorAutoSave::ForceNow();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, CommandHistory::CanUndo()))
                    CommandHistory::Undo();
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, CommandHistory::CanRedo()))
                    CommandHistory::Redo();

                ImGui::Separator();

                Entity sel = EditorSelection::GetSelectedEntity();
                const bool hasSel = sel && sel.IsValid();
                if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSel))
                    CommandHistory::Execute(std::make_unique<EntityDuplicateCommand>(sel.GetScene(), sel));
                if (ImGui::MenuItem("Delete", "Del", false, hasSel))
                    CommandHistory::Execute(std::make_unique<EntityDestroyCommand>(sel.GetScene(), sel));

                ImGui::Separator();

                if (ImGui::MenuItem("Preferences..."))
                    EditorSettingsWindow::Show();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Window")) {
                for (auto& panel : s_Panels)
                    ImGui::MenuItem(panel->GetWindowID(), nullptr, &panel->m_Open);
                ImGui::Separator();

                if (ImGui::BeginMenu("Workspaces")) {
                    auto wss = GetWorkspaces();

                    bool activeIsBuiltin = false;
                    for (const auto& ws : wss) {
                        bool isActive = (s_Settings.activeLayout == ws.name);
                        std::string label = ws.builtin ? (ws.name + "  (builtin)") : ws.name;
                        if (ImGui::MenuItem(label.c_str(), nullptr, isActive))
                            LoadWorkspace(ws.name);
                        if (isActive) activeIsBuiltin = ws.builtin;
                    }
                    if (!wss.empty()) ImGui::Separator();

                    if (ImGui::MenuItem("Save Current As..."))
                        s_ShowSaveWorkspacePopup = true;
                    if (ImGui::MenuItem("Rename Current...", nullptr, false, !activeIsBuiltin))
                        s_ShowRenameWorkspacePopup = true;
                    if (ImGui::MenuItem("Delete Current...", nullptr, false, !activeIsBuiltin))
                        s_ShowDeleteWorkspaceConfirm = true;

                    ImGui::Separator();
                    if (ImGui::MenuItem("Reset to Built-in"))
                        ResetWorkspaceToBuiltin();
                    ImGui::EndMenu();
                }
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
                // Style submenu: deferred to next BeginFrame (font atlas can't change mid-frame)
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
                ImGui::EndMenu();
            }

            ImGui::EndMenuBar();
        }

        // Texture remap popup: deferred open from menu
        if (s_ShowTextureRemapDialog) {
            const ImportReport& report = ModelImporter::GetLastImportReport();
            if (report.HasUnresolved())
                TextureRemapDialog::Open(report);
            s_ShowTextureRemapDialog = false;
        }

        // Workspace Save / Rename / Delete popups rendered outside menu scope so ImGui can track
        // them; deferred-open pattern from Save Layout precedent.
        if (s_ShowSaveWorkspacePopup) {
            ImGui::OpenPopup("Save Workspace");
            s_ShowSaveWorkspacePopup = false;
        }
        if (ImGui::BeginPopupModal("Save Workspace", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            static char nameBuf[128] = "";
            ImGui::Text("Workspace Name:");
            ImGui::SetNextItemWidth(250.0f);
            ImGui::InputText("##SaveWorkspaceName", nameBuf, sizeof(nameBuf));

            ImGui::Spacing();
            if (ImGui::Button("Save", ImVec2(120, 0)) && nameBuf[0] != '\0') {
                SaveWorkspaceAs(nameBuf);
                nameBuf[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                nameBuf[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Rename input pre-fills with the active name on open so the user can edit in place rather
        // than retyping. Static buf is re-seeded each open.
        static char s_RenameBuf[128] = "";
        if (s_ShowRenameWorkspacePopup) {
            std::snprintf(s_RenameBuf, sizeof(s_RenameBuf), "%s", s_Settings.activeLayout.c_str());
            ImGui::OpenPopup("Rename Workspace");
            s_ShowRenameWorkspacePopup = false;
        }
        if (ImGui::BeginPopupModal("Rename Workspace", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("New Name:");
            ImGui::SetNextItemWidth(250.0f);
            ImGui::InputText("##RenameWorkspaceName", s_RenameBuf, sizeof(s_RenameBuf));

            ImGui::Spacing();
            const bool canRename = s_RenameBuf[0] != '\0' && s_RenameBuf != s_Settings.activeLayout;
            if (!canRename) ImGui::BeginDisabled();
            if (ImGui::Button("Rename", ImVec2(120, 0))) {
                RenameWorkspace(s_Settings.activeLayout, s_RenameBuf);
                s_RenameBuf[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            if (!canRename) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                s_RenameBuf[0] = '\0';
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (s_ShowDeleteWorkspaceConfirm) {
            ImGui::OpenPopup("Delete Workspace");
            s_ShowDeleteWorkspaceConfirm = false;
        }
        if (ImGui::BeginPopupModal("Delete Workspace", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete workspace '%s'?", s_Settings.activeLayout.c_str());
            ImGui::TextDisabled("This cannot be undone.");

            ImGui::Spacing();
            if (ImGui::Button("Delete", ImVec2(120, 0))) {
                DeleteWorkspace(s_Settings.activeLayout);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
                ImGui::CloseCurrentPopup();
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
        if (EditorAutoSave::IsNoticeActive()) {
            title += " — ";
            title += EditorAutoSave::GetLastNotice();
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
        // invariant: thumbnails belong to the outgoing project's UUID space; dropped before any
        // reload so PushDeletion fences against the current ImGui pool, and the new project starts
        // with an empty cache.
        UI::ThumbnailCache::Clear();

        // Reload editor settings from the new project directory
        LoadSettings();

        // Load this project's persisted scene-view camera poses so the last-scene auto-load
        // below restores it at the camera it was left at.
        s_SceneViews.Load(SceneViewsPath());

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

        // Auto-load last scene now that AssetDatabase is populated for the new project. SetActiveScene
        // runs at App::App() before LoadProject, so it can't do this (see comment in SetActiveScene).
        // OpenScene fires the autosave recovery scan as a side effect.
        if (s_ActiveScene && !s_Settings.lastSceneUUID.empty())
        {
            UUID uuid = UUID::FromString(s_Settings.lastSceneUUID);
            if (uuid.IsValid() && AssetDatabase::Exists(uuid))
            {
                const auto& meta = AssetDatabase::GetMetadata(uuid);
                if (!meta.Path.empty() && fs::exists(meta.Path))
                    OpenScene(meta.Path);
            }
            s_Settings.lastSceneUUID.clear();
        }

        // Reload the skybox now that the project's asset paths are live. RenderingSystem::ctor runs
        // before any project is loaded, so its IBL init falls back to engine-assets (which don't ship
        // an HDR). Once the project root is set, re-resolve the settings path and reload.
        if (auto rs = SystemRegistry::GetSystem<RenderingSystem>();
            rs && !s_Settings.skyboxPath.empty())
        {
            fs::path skyboxAbsPath = fs::path(s_Settings.skyboxPath).is_absolute()
                ? fs::path(s_Settings.skyboxPath)
                : FileSystem::ResolveAsset(s_Settings.skyboxPath);
            if (fs::exists(skyboxAbsPath))
                rs->ReloadSkybox(skyboxAbsPath);
        }

        // Re-hydrate thumbnail cache from the new project's <project>/.luth/thumbnails/. AssetDatabase
        // has been fully reloaded by LoadProject by this point, so orphan-GC has the stable registry
        // it needs.
        UI::ThumbnailCache::ScanDiskCache();

        // Broadcast project switch to panels. Path stays empty when called from shutdown / unload
        // (no project loaded yet); subscribers should treat empty path as "project unloaded" rather
        // than "default project."
        const std::string projPath = FileSystem::ProjectPath().string();
        const std::string projName = FileSystem::ProjectPath().filename().string();
        EventBus::Enqueue<ProjectChangedSignal>(BusType::MainThread, projPath, projName);

        LH_LOG(Editor, info, "Editor: Project changed, panels refreshed");
    }
}
