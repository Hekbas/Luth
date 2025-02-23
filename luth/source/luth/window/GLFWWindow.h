#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/window/Window.h"

#include <GLFW/glfw3.h>

namespace Luth
{
    class GLFWWindow : public Window
    {
    public:
        GLFWWindow(const WindowSpec& spec);
        virtual ~GLFWWindow();

        void OnUpdate() override;
        void SwapBuffers();

        void SetVSync(bool enabled) override;
        void ToggleFullscreen() override;

        u32 GetWidth() const override { return m_Data.Width; }
        u32 GetHeight() const override { return m_Data.Height; }
        virtual void* GetHandle() const override { return m_Window; }

    private:
        void Init(const WindowSpec& spec);
        void Shutdown();

        GLFWwindow* m_Window = nullptr;
        std::unique_ptr<Renderer> m_Renderer;

        struct WindowData
        {
            std::string Title;
            u32 Width;
            u32 Height;
            bool VSync;
            bool Fullscreen;
        };

        WindowData m_Data;
    };
}