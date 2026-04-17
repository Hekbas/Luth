#pragma once

#include "luth/events/Event.h"
#include <GLFW/glfw3.h>

namespace Luth
{
    class MouseMovedEvent : public Event
    {
    public:
        MouseMovedEvent(float x, float y) : m_X(x), m_Y(y) {}

        float GetX() const { return m_X; }
        float GetY() const { return m_Y; }
        virtual const char* GetName() const override { return "MouseMovedEvent"; }
        virtual u32 GetCategoryFlags() const override {
            return EventCategoryMouse | EventCategoryInput;
        }

    private:
        float m_X, m_Y;
    };

    class MouseScrolledEvent : public Event
    {
    public:
        MouseScrolledEvent(float xOffset, float yOffset)
            : m_XOffset(xOffset), m_YOffset(yOffset) {}

        float GetXOffset() const { return m_XOffset; }
        float GetYOffset() const { return m_YOffset; }

        virtual const char* GetName() const override { return "MouseScrolledEvent"; }
        virtual u32 GetCategoryFlags() const override {
            return EventCategoryMouse | EventCategoryInput;
        }
        static const char* GetStaticName() { return "MouseScrolledEvent"; }

    private:
        float m_XOffset, m_YOffset;
    };

    class MouseButtonEvent : public Event
    {
    public:
        int GetButton() const { return m_Button; }
        int GetAction() const { return m_Action; }
        virtual u32 GetCategoryFlags() const override {
            return EventCategoryMouse | EventCategoryMouseButton | EventCategoryInput;
        }

    protected:
        MouseButtonEvent(int button, int action)
            : m_Button(button), m_Action(action) {}

        int m_Button, m_Action;
    };

    class MouseButtonPressedEvent : public MouseButtonEvent
    {
    public:
        MouseButtonPressedEvent(int button)
            : MouseButtonEvent(button, GLFW_PRESS) {}
        virtual const char* GetName() const override { return "MouseButtonPressedEvent"; }
    };

    class MouseButtonReleasedEvent : public MouseButtonEvent {
    public:
        MouseButtonReleasedEvent(int button)
            : MouseButtonEvent(button, GLFW_RELEASE) {}
        virtual const char* GetName() const override { return "MouseButtonReleasedEvent"; }
    };
}
