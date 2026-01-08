#pragma once

#include "luth/ECS/System.h"

namespace Luth
{
    class AnimationSystem : public System
    {
    public:
        AnimationSystem() = default;

        void Update(entt::registry& registry) override {}
    };
}
