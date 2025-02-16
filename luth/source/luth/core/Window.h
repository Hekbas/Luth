#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/events/Event.h"

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
        using EventCallbackFn = std::function<void(Event&)>;

        Window(const WindowSpec& spec = WindowSpec());
        ~Window();

        void OnUpdate();
        void SetEventCallback(const EventCallbackFn& callback);

        // Window attributes
        u32 GetWidth() const { return m_Data.Width; }
        u32 GetHeight() const { return m_Data.Height; }
        void* GetNativeWindow() const { return m_Window; }

        // Feature control
        void SetVSync(bool enabled);
        bool IsVSync() const { return m_Data.VSync; }
        void ToggleFullscreen();

    private:
        void Init();
        void Shutdown();

        GLFWwindow* m_Window;
        WindowSpec m_Spec;

        struct WindowData
        {
            std::string Title;
            u32 Width, Height;
            bool VSync;
            EventCallbackFn EventCallback;
        };
        WindowData m_Data;
    };
}
