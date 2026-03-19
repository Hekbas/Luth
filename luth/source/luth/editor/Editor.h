#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/platform/Window.h"
#include "luth/scene/Scene.h"

#include <memory>
#include <filesystem>
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
            for (auto& panel : s_Panels) {
                if (auto found = dynamic_cast<T*>(panel.get()))
                    return found;
            }
            return nullptr;
        }

        static bool ApplyRandomStyle();
        static void SetCustomStyle();
        static void SetBubblegumStyle();
		static void SetMatrixStyle();
        static void SetRandomStyle();

        static ImFont* GetMainFont() { return m_MainFont; }
        static ImFont* GetFARegular() { return m_FARegular; }
        static ImFont* GetFASolid() { return m_FASolid; }

        // Scene management
        static void SetActiveScene(std::shared_ptr<Scene> scene);
        static void NewScene();
        static void OpenScene();
        static void OpenScene(const std::filesystem::path& path);
        static void SaveScene();
        static void SaveSceneAs();
        static void MarkDirty();
        static bool IsDirty() { return s_IsDirty; }

    private:
        static void ProcessShortcuts();
        static void DrawMenuBar();
        static void UpdateWindowTitle();
        static inline Window* s_Window = nullptr;
        static inline ImGuiContext* s_Context = nullptr;
        static inline VkDescriptorPool s_ImGuiPool = VK_NULL_HANDLE;
        static inline std::vector<std::unique_ptr<Panel>> s_Panels;

        static inline ImFont* m_MainFont = nullptr;
        static inline ImFont* m_FARegular = nullptr;
        static inline ImFont* m_FASolid = nullptr;

        // Scene state
        static inline std::shared_ptr<Scene> s_ActiveScene;
        static inline std::filesystem::path s_ScenePath;
        static inline bool s_IsDirty = false;
        static inline u32 s_LastHierarchyVersion = 0;
    };
}
