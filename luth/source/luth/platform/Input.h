#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/events/KeyEvent.h"
#include "luth/events/MouseEvent.h"

#include <array>

namespace Luth
{
    // Polling shim over the EventBus key and mouse streams. Init subscribes once; gameplay then
    // queries IsKeyPressed / GetMousePosition synchronously. Reads are game-stage-only — the cached
    // arrays are written exclusively from main-thread event handlers, so no atomic dance is needed.
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
