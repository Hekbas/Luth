#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/window/Window.h"

#include <memory>
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

        static ImFont* GetIconFont() { return m_IconFont; }

    private:
        static inline ImGuiContext* s_Context = nullptr;
        static inline std::vector<std::unique_ptr<Panel>> s_Panels;
        static inline ImFont* m_IconFont;
    };
}
