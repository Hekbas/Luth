#pragma once

#include "Event.h"

namespace Luth
{
    class RenderResizeEvent : public Event
    {
    public:
        RenderResizeEvent(u32 width, u32 height)
            : m_Width(width), m_Height(height) {}

        virtual const char* GetName() const override { return "RenderResizeEvent"; }
        virtual u32 GetCategoryFlags() const override { return EventCategoryRender; }

        static const char* GetStaticName() { return "RenderResizeEvent"; }

        u32 GetWidth() const { return m_Width; }
        u32 GetHeight() const { return m_Height; }

    private:
        u32 m_Width;
        u32 m_Height;
    };
}
