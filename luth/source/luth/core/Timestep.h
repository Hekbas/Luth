#pragma once

#include "luth/core/LuthTypes.h"

#include <GLFW/glfw3.h>

namespace Luth
{
    class Timestep
    {
    public:
        static f32 GetTime() { return glfwGetTime(); }
        static f32 GetTimeMS() { return glfwGetTime() * 1000.0f; }
    };
}
