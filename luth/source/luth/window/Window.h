#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/Renderer.h"

#include <functional>
#include <memory>

namespace Luth
{
    struct WindowSpec
    {
        std::string Title = "Luth Engine";
        u32 Width = 1280;
        u32 Height = 720;
        bool VSync = false;
        bool Fullscreen = false;
    };

    class Window
    {
    public:
        virtual ~Window() = default;

        virtual void OnUpdate() = 0;
        virtual void SwapBuffers() = 0;

        virtual void SetVSync(bool enabled) = 0;;
        virtual void ToggleFullscreen() = 0;

        virtual u32 GetWidth() const = 0;
        virtual u32 GetHeight() const = 0;
        virtual void* GetNativeWindow() const = 0;

        static std::unique_ptr<Window> Create(const WindowSpec& spec = WindowSpec());
    };
}
