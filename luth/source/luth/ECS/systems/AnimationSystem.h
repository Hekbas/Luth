#pragma once

#include "luth/core/Time.h"
#include "luth/ECS/System.h"
#include "luth/ECS/components.h"
#include "luth/renderer/SkinnedModel.h"
#include "luth/resources/libraries/ModelLibrary.h"

namespace Luth
{
    class AnimationSystem : public System
    {
    public:
        void Update(entt::registry& registry) override
        {
            auto view = registry.view<Animation>();
            for (auto [entity, anim] : view.each()) {
                std::shared_ptr<SkinnedModel> skinned =
                    std::dynamic_pointer_cast<SkinnedModel>(ModelLibrary::Get(anim.ModelUUID));
				skinned->UpdateAnimation(Time::DeltaTime());
            }
        }
    };
}
