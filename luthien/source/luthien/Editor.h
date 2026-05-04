#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/memory/LinearAllocator.h"
#include "luth/platform/Window.h"
#include "luth/scene/Scene.h"
#include "luthien/EditorSettings.h"
#include "luthien/ProjectLauncher.h"
#include "luthien/Workspace.h"

#include <memory>
#include <filesystem>
#include <typeindex>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <imgui/imgui_internal.h>

struct ImGuiContext;

namespace Luth::JobSystem
{
    struct JobArgs;   // luth/jobs/JobSystem.h — full def only needed in Editor.cpp
}

namespace Luth
{
    // Forward decls for the new Gather/Draw lifecycle. Defined in EditorSnapshot.h
    // and events/EditorSignals.h (added in sub-tasks B and the editor-signal-bus epic
    // respectively). Panel's hooks take these by reference; full definitions only
    // needed in panel .cpp files that override OnGather/OnDraw/OnEvent.
    class EditorSnapshot;
    class EditorSnapshotBuilder;
    struct EditorSignal;

    // Editor panel base. Gather/Draw lifecycle (since v2.9.0 editor-foundation):
    //   OnInit       — once after construction; subscribe to signals here.
    //   OnGather     — worker fiber, no ImGui, no Vk; fills m_SnapshotFragment.
    //   OnDraw       — main thread, the only place ImGui calls are legal; reads frozen snapshot.
    //   OnEvent      — main thread between frames (EventBus drain); panel-state mutations.
    //   OnShutdown   — editor teardown.
    class Panel
    {
    public:
        virtual ~Panel() = default;

        virtual void OnInit() {}
        virtual void OnGather(EditorSnapshotBuilder& /*builder*/) {}
        virtual void OnDraw(const EditorSnapshot& snapshot) = 0;
        virtual void OnEvent(const EditorSignal& /*signal*/) {}
        virtual void OnShutdown() {}

        // Introspection — Editor populates these; panels read.
        bool IsVisible() const { return m_Visible; }
        bool IsFocused() const { return m_Focused; }
        bool IsDocked()  const { return m_Docked;  }
        int  GetWindowFlags() const { return m_WindowFlags; }
        const char* GetWindowID() const { return m_WindowID; }   // must be string literal

        // Wraps ImGui::Begin and updates introspection. Panels call this from OnDraw
        // instead of ImGui::Begin directly. Caller still pairs with ImGui::End.
        // Visibility flows back into the gather-dispatch loop next frame: invisible
        // panels skip OnGather, accepting one frame stale on visibility resume.
        bool BeginWindow(const char* name, ImGuiWindowFlags flags = 0)
        {
            bool open = ImGui::Begin(name, nullptr, flags);
            m_Visible = open;
            m_Focused = open && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootWindow);
            m_Docked  = open && ImGui::IsWindowDocked();
            return open;
        }

        // Overload that wires the title-bar X to a persistent open flag (typically
        // &m_Open). Panels using this opt into the Window menu's toggle path.
        bool BeginWindow(const char* name, bool* p_open, ImGuiWindowFlags flags = 0)
        {
            bool open = ImGui::Begin(name, p_open, flags);
            m_Visible = open;
            m_Focused = open && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootWindow);
            m_Docked  = open && ImGui::IsWindowDocked();
            return open;
        }

    protected:
        friend class Editor;
        friend class EditorSnapshotBuilder;   // writes m_GatherAlloc / m_SnapshotFragment / m_FragmentType

        // m_Open is the persistent user choice (Window menu, close X). m_Visible
        // is per-frame ImGui state (collapsed / off-screen tab / etc.).
        bool m_Open    = true;
        bool m_Visible = true;
        bool m_Focused = false;
        bool m_Docked  = false;
        int  m_WindowFlags = 0;
        const char* m_WindowID = "Panel";

        // Error-boundary state — see Editor::DrawPanelGuarded.
        bool m_Crashed = false;
        u8   m_CrashStreak = 0;

        // Per-panel scratch for OnGather. Reset by gather thunk before each call.
        // Pages tracked under Memory::Category::FrameLinear (LinearAllocator's hardcoded
        // category). Panel-lifetime allocations (the panel object, persistent caches)
        // use LH_NEW(Memory::Category::Editor, ...) at their construction sites instead.
        Memory::LinearAllocator m_GatherAlloc{ 64 * 1024 };
        void* m_SnapshotFragment = nullptr;
        std::type_index m_FragmentType{ typeid(void) };
    };

    class Editor
    {
    public:
        static void Init(Window* window);
        static void Shutdown();

        static void BeginFrame();
        static void EndFrame();
        static void Render();

        static bool WantCaptureMouse();
        static bool WantCaptureKeyboard();

        static void AddPanel(Panel* panel);

        template<typename T>
        static T* GetPanel() {
            auto it = s_PanelRegistry.find(std::type_index(typeid(T)));
            return it != s_PanelRegistry.end() ? static_cast<T*>(it->second) : nullptr;
        }

        // Deferred style change — applied on next BeginFrame (font atlas can't
        // rebuild mid-frame). Accepts a built-in name (Custom/Bubblegum/Matrix/
        // Rider) or an absolute JSON path.
        static void LoadStyle(const std::string& nameOrPath);

        // Easter egg — randomised color palette. Unrelated to LoadStyle.
        static void SetRandomStyle();

        static ImFont*  GetMainFont()        { return m_MainFont; }
        static ImFont*  GetFARegular()       { return m_FARegular; }
        static ImFont*  GetFASolid()         { return m_FASolid; }
        static ImFont*  GetFARegularLarge()  { return m_FARegularLarge; }
        static ImFont*  GetFASolidLarge()    { return m_FASolidLarge; }
        static ImFont*& MainFontRef()        { return m_MainFont; }
        static ImFont*& FARegularRef()       { return m_FARegular; }
        static ImFont*& FASolidRef()         { return m_FASolid; }
        static ImFont*& FARegularLargeRef()  { return m_FARegularLarge; }
        static ImFont*& FASolidLargeRef()    { return m_FASolidLarge; }

        // Scene management
        static void SetActiveScene(std::shared_ptr<Scene> scene);
        static std::shared_ptr<Scene> GetActiveScene() { return s_ActiveScene; }
        static const std::filesystem::path& GetScenePath() { return s_ScenePath; }
        static void NewScene();
        static void OpenScene();
        static void OpenScene(const std::filesystem::path& path);
        static void SaveScene();
        static void SaveSceneAs();
        static void MarkDirty();
        static bool IsDirty() { return s_IsDirty; }
        // Resets dirty flag + hierarchy version together. Used after scene
        // load and play-mode Stop to prevent the load itself from bumping
        // s_IsDirty via the hierarchy-version delta check.
        static void ResetDirtyState(bool dirty = false);

        // Project switching
        static void ShowProjectLauncher();
        static void OnProjectChanged();

        // Settings & Layout
        static EditorSettings& GetSettings() { return s_Settings; }
        static void LoadSettings();
        static void SaveSettings();
        // Push live state from s_Settings into panels (camera params, skybox, etc.).
        // Public so EditorSettingsWindow can re-sync after a Preferences edit.
        static void ApplyPersistence();
        // Workspace = ImGui dock layout (.ini) + sidecar JSON (per-panel visibility).
        // Built-ins live under FileSystem::EngineAssetsPath("workspaces"); user copies
        // under runtime/layouts/. Built-in name shadows user copy of the same name.
        static bool LoadWorkspace(const std::string& name);
        static bool SaveWorkspaceAs(const std::string& name);
        static bool RenameWorkspace(const std::string& oldName, const std::string& newName);
        static bool DeleteWorkspace(const std::string& name);
        static bool ResetWorkspaceToBuiltin();
        static std::vector<WorkspaceInfo> GetWorkspaces();

    private:
        static void InitImGui(Window* window);
        static void InitPanels();

        static void ProcessShortcuts();
        static void DrawMenuBar();
        static void UpdateWindowTitle();

        // Gather thunk dispatched onto worker fibers. Resets the panel's scratch,
        // calls OnGather, catches exceptions, dumps a stack trace, and bumps
        // m_CrashStreak. m_CrashStreak >= 3 → m_Crashed, panel goes dark until reset.
        static void GatherJobThunk(JobSystem::JobArgs args);

        // Main-thread guard around panel->OnDraw. Mirrors GatherJobThunk's catch
        // contract so a thrown OnDraw can't take the editor down.
        static void DrawPanelGuarded(Panel* panel, const EditorSnapshot& snapshot);
        static void DrawCrashedPlaceholder(Panel* panel);
        static inline Window* s_Window = nullptr;
        static inline ImGuiContext* s_Context = nullptr;
        static inline VkDescriptorPool s_ImGuiPool = VK_NULL_HANDLE;
        static inline std::vector<std::unique_ptr<Panel>> s_Panels;
        static inline std::unordered_map<std::type_index, Panel*> s_PanelRegistry;

        static inline ImFont* m_MainFont        = nullptr;
        static inline ImFont* m_FARegular       = nullptr;
        static inline ImFont* m_FASolid         = nullptr;
        static inline ImFont* m_FARegularLarge  = nullptr;   // 64 px FA-Regular for large icons (ProjectPanel grid empty-folder)
        static inline ImFont* m_FASolidLarge    = nullptr;   // 64 px FA-Solid for large icons (ProjectPanel grid)

        // Scene state
        static inline std::shared_ptr<Scene> s_ActiveScene;
        static inline std::filesystem::path s_ScenePath;
        static inline bool s_IsDirty = false;

        // Settings
        static inline EditorSettings s_Settings;
        static inline std::filesystem::path s_SettingsPath;

        // Deferred style change (fonts can't be rebuilt mid-frame)
        static inline std::string s_PendingStyle;

        // Layout popup state
        static inline bool s_ShowSaveLayoutPopup = false;

        // First-run default-layout snapshot — set in Init when layouts/Default.ini
        // is missing, consumed at end of the first Render once ImGui has populated
        // dock state.
        static inline bool s_NeedDefaultLayoutSave = false;

        // Deferred LoadWorkspace(activeLayout) — set in Init, consumed at end of the
        // first Render so panels and ImGui dock state exist before we apply.
        static inline bool s_NeedActiveWorkspaceLoad = false;

        // Texture remap dialog state (deferred open from menu)
        static inline bool s_ShowTextureRemapDialog = false;

        // Project launcher
        static inline bool s_ShowLauncher = false;
    };
}
