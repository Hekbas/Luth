#pragma once

#include "luth/core/LuthTypes.h"

#include <GLFW/glfw3.h>
#include <functional>

namespace Luth
{
    struct WindowSpec
    {
        std::string Title = "Luth Engine";
        u32 Width = 1280;
        u32 Height = 720;
        bool VSync = true;
        bool Fullscreen = false;
    };

    class Window
    {
    public:
        Window(const WindowSpec& spec = WindowSpec());
        ~Window();

        void OnUpdate();
        void SwapBuffers();

        void SetVSync(bool enabled);
        void ToggleFullscreen();

        u32 GetWidth() const { return m_Spec.Width; }
        u32 GetHeight() const { return m_Spec.Height; }
        GLFWwindow* GetNativeWindow() const { return m_Window; }

    private:
        void Init();
        void Shutdown();

        WindowSpec m_Spec;
        GLFWwindow* m_Window = nullptr;
    };
}
