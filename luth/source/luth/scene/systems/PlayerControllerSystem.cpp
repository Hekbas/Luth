#include "luthpch.h"

#include "luth/scene/systems/PlayerControllerSystem.h"

#include "luth/scene/Scene.h"
#include "luth/scene/components/Physics.h"
#include "luth/scene/components/Transform.h"
#include "luth/platform/Input.h"
#include "luth/core/diagnostics/Profiler.h"

namespace Luth
{
    namespace
    {
        // Raw GLFW keycodes — Luth has no Key enum yet, and existing Input call sites use the same
        // literals. Lift to a Key constant set when one lands.
        constexpr int kKeyW     = 87;
        constexpr int kKeyA     = 65;
        constexpr int kKeyS     = 83;
        constexpr int kKeyD     = 68;
        constexpr int kKeySpace = 32;
    }

    void PlayerControllerSystem::Update(Scene* scene)
    {
        LH_PROFILE_FUNCTION();
        if (!scene) return;

        auto& reg = scene->Registry();
        auto view = reg.view<Component::CharacterController, Component::WorldTransform>();
        for (auto entity : view)
        {
            auto& cc          = view.get<Component::CharacterController>(entity);
            const auto& world = view.get<Component::WorldTransform>(entity);

            // GLM is column-major, right-handed, Y-up. Forward = -Z basis, right = +X basis. Extract
            // from the world matrix so a parented character (e.g. on a moving platform) still walks
            // along its local frame. World basis vectors carry any scale on the entity; we project to
            // the ground plane and normalise so WASD speed stays uniform regardless of pitch/scale.
            const Vec3 forward = -Vec3(world.Matrix[2]);
            const Vec3 right   =  Vec3(world.Matrix[0]);

            Vec3 fwdH(forward.x, 0.0f, forward.z);
            Vec3 rgtH(right.x,   0.0f, right.z);
            if (Math::Length2(fwdH) > Math::SmallNumber<f32>) fwdH = Math::Normalize(fwdH);
            if (Math::Length2(rgtH) > Math::SmallNumber<f32>) rgtH = Math::Normalize(rgtH);

            Vec3 desired(0.0f);
            if (Input::IsKeyPressed(kKeyW)) desired += fwdH;
            if (Input::IsKeyPressed(kKeyS)) desired -= fwdH;
            if (Input::IsKeyPressed(kKeyD)) desired += rgtH;
            if (Input::IsKeyPressed(kKeyA)) desired -= rgtH;

            if (Math::Length2(desired) > Math::SmallNumber<f32>)
                desired = Math::Normalize(desired) * cc.moveSpeed;

            cc.SetDesiredVelocity(desired);

            if (Input::IsKeyPressed(kKeySpace) && cc.IsGrounded())
                cc.Jump();
        }
    }
}
