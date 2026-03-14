#pragma once

#include <entt/entt.hpp>

namespace Luth
{
    class Scene;

    class System
    {
    public:
        virtual ~System() = default;
        virtual void Update(Scene* scene) = 0;
    };
}
