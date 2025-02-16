#pragma once

#include "luth/core/LuthTypes.h"

namespace Luth
{
    class Event
    {
    public:
        virtual ~Event() = default;
        bool m_Handled = false;
    };
}
