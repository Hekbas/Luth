#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/platform/Window.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

namespace Luth
{
    class WinWindow : public Window
    {
    public:
        WinWindow(const WindowSpec& spec);
        virtual ~WinWindow();

        void OnUpdate() override;

        void SetVSync(bool enabled) override;
        void ToggleFullscreen() override;

        u32 GetWidth() const override { return m_Data.Width; }
        u32 GetHeight() const override { return m_Data.Height; }
        void* GetNativeWindow() const override { return m_GLFWwindow; }

        void SetWindowColors(const Vec3& caption, const Vec3& border, const Vec3& text) override;
        void SetWindowIcon(GLFWwindow* window, fs::path iconPath);

        bool IsMinimized() override;

        void Shutdown() override;

    private:
        void Init(const WindowSpec& spec);

        GLFWwindow* m_GLFWwindow = nullptr;

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