#pragma once

#include "luth/core/LuthTypes.h"

struct ImGuiContext;

namespace Luth
{
    class Editor
    {
    public:
        static void Init(void* window);
        static void Shutdown();

        static void BeginFrame();
        static void EndFrame();

        static bool WantCaptureMouse();
        static bool WantCaptureKeyboard();

    private:
        static inline ImGuiContext* s_Context = nullptr;
    };
}
