#include "luthpch.h"
#include "luth/platform/Input.h"
#include "luth/editor/Editor.h"
#include "luth/platform/EventBus.h"

namespace Luth
{
    void Input::Init()
    {
        s_KeyData.fill(false);
        s_MouseData.fill(false);
        s_MousePos = { 0.0f, 0.0f };

        EventBus::Subscribe<KeyPressedEvent>(BusType::MainThread, [](Event& e) {
            OnKeyPressed(static_cast<KeyPressedEvent&>(e));
        });

        EventBus::Subscribe<KeyReleasedEvent>(BusType::MainThread, [](Event& e) {
            OnKeyReleased(static_cast<KeyReleasedEvent&>(e));
        });

        EventBus::Subscribe<MouseButtonPressedEvent>(BusType::MainThread, [](Event& e) {
            OnMouseButtonPressed(static_cast<MouseButtonPressedEvent&>(e));
        });

        EventBus::Subscribe<MouseButtonReleasedEvent>(BusType::MainThread, [](Event& e) {
            OnMouseButtonReleased(static_cast<MouseButtonReleasedEvent&>(e));
        });

        EventBus::Subscribe<MouseMovedEvent>(BusType::MainThread, [](Event& e) {
            OnMouseMoved(static_cast<MouseMovedEvent&>(e));
        });
        
        LH_CORE_INFO("Input System Initialized");
    }

    bool Input::IsKeyPressed(int keycode)
    {
        if (Editor::WantCaptureKeyboard()) return false;
        if (keycode >= 0 && keycode < 512)
            return s_KeyData[keycode];
        return false;
    }

    bool Input::IsMouseButtonPressed(int button)
    {
        if (Editor::WantCaptureMouse()) return false;
        if (button >= 0 && button < 8)
            return s_MouseData[button];
        return false;
    }

    Vec2 Input::GetMousePosition()
    {
        return s_MousePos;
    }

    float Input::GetMouseX() { return s_MousePos.x; }
    float Input::GetMouseY() { return s_MousePos.y; }

    bool Input::OnKeyPressed(KeyPressedEvent& e) {
        if (e.GetKeyCode() < 512) s_KeyData[e.GetKeyCode()] = true;
        return false;
    }
    bool Input::OnKeyReleased(KeyReleasedEvent& e) {
        if (e.GetKeyCode() < 512) s_KeyData[e.GetKeyCode()] = false;
        return false;
    }
    bool Input::OnMouseButtonPressed(MouseButtonPressedEvent& e) {
        if (e.GetButton() < 8) s_MouseData[e.GetButton()] = true;
        return false;
    }
    bool Input::OnMouseButtonReleased(MouseButtonReleasedEvent& e) {
        if (e.GetButton() < 8) s_MouseData[e.GetButton()] = false;
        return false;
    }
    bool Input::OnMouseMoved(MouseMovedEvent& e) {
        s_MousePos = { e.GetX(), e.GetY() };
        return false;
    }
}
