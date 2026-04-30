#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/platform/Window.h"

#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#ifdef None
#undef None
#endif


namespace Luth
{
    class LinWindow : public Window
    {
    public:
        LinWindow(const WindowSpec& spec);
        virtual ~LinWindow();

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
