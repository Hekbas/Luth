#pragma once

#include "luth/core/LuthTypes.h"

namespace Luth
{
    enum EventCategory
    {
        None = 0,
        EventCategoryApplication    = 1 << 0,
        EventCategoryInput          = 1 << 1,
        EventCategoryKeyboard       = 1 << 2,
        EventCategoryMouse          = 1 << 3,
        EventCategoryMouseButton    = 1 << 4,
        EventCategoryFileDrop       = 1 << 5
    };

    class Event
    {
    public:
        virtual ~Event() = default;
        virtual const char* GetName() const = 0;
        virtual u32 GetCategoryFlags() const = 0;

        bool IsInCategory(EventCategory category) const {
            return GetCategoryFlags() & category;
        }

        bool m_Handled = false;
    };
}
