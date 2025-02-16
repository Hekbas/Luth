#pragma once

#include "Event.h"

namespace Luth
{
    class KeyEvent : public Event
    {
    public:
        u32 GetKeyCode() const { return m_KeyCode; }
        virtual u32 GetCategoryFlags() const override {
            return EventCategoryKeyboard | EventCategoryInput;
        }

    protected:
        KeyEvent(u32 keycode) : m_KeyCode(keycode) {}
        u32 m_KeyCode;
    };

    class KeyPressedEvent : public KeyEvent
    {
    public:
        KeyPressedEvent(u32 keycode, u32 repeatCount)
            : KeyEvent(keycode), m_RepeatCount(repeatCount) {}

        u32 GetRepeatCount() const { return m_RepeatCount; }

        virtual const char* GetName() const override { return "KeyPressedEvent"; }
        static const char* GetStaticName() { return "KeyPressedEvent"; }

    private:
        u32 m_RepeatCount;
    };

    class KeyReleasedEvent : public KeyEvent
    {
    public:
        KeyReleasedEvent(u32 keycode) : KeyEvent(keycode) {}

        virtual const char* GetName() const override { return "KeyReleasedEvent"; }
        static const char* GetStaticName() { return "KeyReleasedEvent"; }
    };
}
