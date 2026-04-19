#pragma once

#include "luth/core/types/LuthTypes.h"
#include "luth/platform/Window.h"
#include "luth/scene/Scene.h"
#include "luthien/EditorSettings.h"
#include "luthien/ProjectLauncher.h"

#include <memory>
#include <filesystem>
#include <typeindex>
#include <unordered_map>
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <imgui/imgui_internal.h>

struct ImGuiContext;

namespace Luth
{
    class Panel
    {
    public:
        virtual ~Panel() = default;
        virtual void OnInit() = 0;
        virtual void OnRender() = 0;
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

        static ImFont*  GetMainFont()     { return m_MainFont; }
        static ImFont*  GetFARegular()    { return m_FARegular; }
        static ImFont*  GetFASolid()      { return m_FASolid; }
        static ImFont*& MainFontRef()     { return m_MainFont; }
        static ImFont*& FARegularRef()    { return m_FARegular; }
        static ImFont*& FASolidRef()      { return m_FASolid; }

        // Scene management
        static void SetActiveScene(std::shared_ptr<Scene> scene);
        static void NewScene();
        static void OpenScene();
        static void OpenScene(const std::filesystem::path& path);
        static void SaveScene();
        static void SaveSceneAs();
        static void MarkDirty();
        static bool IsDirty() { return s_IsDirty; }

        // Project switching
        static void ShowProjectLauncher();
        static void OnProjectChanged();

        // Settings & Layout
        static EditorSettings& GetSettings() { return s_Settings; }
        static void LoadSettings();
        static void SaveSettings();
        static void SaveLayout(const std::string& name);
        static void LoadLayout(const std::string& name);
        static std::vector<std::string> GetLayoutNames();

    private:
        static void InitImGui(Window* window);
        static void InitPanels();
        static void ApplyPersistence();

        static void ProcessShortcuts();
        static void DrawMenuBar();
        static void UpdateWindowTitle();
        static inline Window* s_Window = nullptr;
        static inline ImGuiContext* s_Context = nullptr;
        static inline VkDescriptorPool s_ImGuiPool = VK_NULL_HANDLE;
        static inline std::vector<std::unique_ptr<Panel>> s_Panels;
        static inline std::unordered_map<std::type_index, Panel*> s_PanelRegistry;

        static inline ImFont* m_MainFont = nullptr;
        static inline ImFont* m_FARegular = nullptr;
        static inline ImFont* m_FASolid = nullptr;

        // Scene state
        static inline std::shared_ptr<Scene> s_ActiveScene;
        static inline std::filesystem::path s_ScenePath;
        static inline bool s_IsDirty = false;
        static inline u32 s_LastHierarchyVersion = 0;

        // Settings
        static inline EditorSettings s_Settings;
        static inline std::filesystem::path s_SettingsPath;

        // Deferred style change (fonts can't be rebuilt mid-frame)
        static inline std::string s_PendingStyle;

        // Layout popup state
        static inline bool s_ShowSaveLayoutPopup = false;

        // Texture remap dialog state (deferred open from menu)
        static inline bool s_ShowTextureRemapDialog = false;

        // Project launcher
        static inline bool s_ShowLauncher = false;
    };
}
