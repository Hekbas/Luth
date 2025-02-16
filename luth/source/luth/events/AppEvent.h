#pragma once

#include "Event.h"

namespace Luth
{
    class WindowResizeEvent : public Event
    {
    public:
        WindowResizeEvent(u32 width, u32 height)
            : m_Width(width), m_Height(height) {}

        u32 GetWidth() const { return m_Width; }
        u32 GetHeight() const { return m_Height; }

        virtual const char* GetName() const override { return "WindowResizeEvent"; }
        virtual u32 GetCategoryFlags() const override { return EventCategoryApplication; }

        static const char* GetStaticName() { return "WindowResizeEvent"; }

    private:
        u32 m_Width, m_Height;
    };

    class WindowCloseEvent : public Event
    {
    public:
        WindowCloseEvent() = default;

        virtual const char* GetName() const override { return "WindowCloseEvent"; }
        virtual u32 GetCategoryFlags() const override { return EventCategoryApplication; }

        static const char* GetStaticName() { return "WindowCloseEvent"; }
    };
}
