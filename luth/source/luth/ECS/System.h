#pragma once

#include <entt/entt.hpp>

namespace Luth
{
    class System
    {
    public:
        virtual ~System() = default;
        virtual void Update(entt::registry& registry) = 0;
    };
}
