#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/platform/KeyEvent.h"
#include "luth/platform/MouseEvent.h"

#include <array>

namespace Luth
{
    class Input
    {
    public:
        static void Init();

        static bool IsKeyPressed(int keycode);
        static bool IsMouseButtonPressed(int button);

        static Vec2 GetMousePosition();
        static float GetMouseX();
        static float GetMouseY();

    private:
        static bool OnKeyPressed(KeyPressedEvent& e);
        static bool OnKeyReleased(KeyReleasedEvent& e);
        static bool OnMouseButtonPressed(MouseButtonPressedEvent& e);
        static bool OnMouseButtonReleased(MouseButtonReleasedEvent& e);
        static bool OnMouseMoved(MouseMovedEvent& e);

        static inline std::array<bool, 512> s_KeyData = { false };
        static inline std::array<bool, 8> s_MouseData = { false };
        static inline Vec2 s_MousePos = { 0.0f, 0.0f };
    };
}
