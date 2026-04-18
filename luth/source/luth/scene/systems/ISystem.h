#pragma once

#include <entt/entt.hpp>

namespace Luth
{
    class Scene;

    class ISystem
    {
    public:
        virtual ~ISystem() = default;
        virtual void Update(Scene* scene) = 0;
    };
}
