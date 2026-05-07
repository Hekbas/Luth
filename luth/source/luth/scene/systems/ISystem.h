#pragma once

#include <entt/entt.hpp>

namespace Luth
{
    class Scene;

    // Base interface for ECS systems registered with SystemRegistry. App::Run invokes Update()
    // once per stage tick on a game or render fiber, depending on which Stage the system was
    // registered under.
    class ISystem
    {
    public:
        virtual ~ISystem() = default;
        virtual void Update(Scene* scene) = 0;
    };
}
