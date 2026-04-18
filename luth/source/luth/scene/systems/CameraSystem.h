#pragma once

#include "luth/scene/systems/ISystem.h"
#include "luth/scene/Components.h"
#include "luth/core/diagnostics/Profiler.h"

namespace Luth
{
    class CameraSystem : public ISystem
    {
    public:
        void Update(Scene* scene) override
        {
            LH_PROFILE_FUNCTION();
            auto& registry = scene->Registry();
            auto view = registry.view<Component::Camera, Component::WorldTransform>();
            for (auto entity : view)
            {
                auto [camera, transform] = view.get<Component::Camera, Component::WorldTransform>(entity);

                if (camera.IsDirty)
                {
                    if (camera.Projection == Component::Camera::ProjectionType::Perspective)
                    {
                        camera.ProjectionMatrix = Math::Perspective(Math::Radians(camera.VerticalFOV), camera.AspectRatio, camera.NearClip, camera.FarClip);
                    }
                    else
                    {
                        float orthoLeft = -camera.OrthographicSize * camera.AspectRatio * 0.5f;
                        float orthoRight = camera.OrthographicSize * camera.AspectRatio * 0.5f;
                        float orthoBottom = -camera.OrthographicSize * 0.5f;
                        float orthoTop = camera.OrthographicSize * 0.5f;

                        camera.ProjectionMatrix = Math::Ortho(orthoLeft, orthoRight, orthoBottom, orthoTop, camera.OrthographicNear, camera.OrthographicFar);
                    }
                    
                    // Vulkan clip space Y flip
                    // We do this here so it's cached in the component
                    camera.ProjectionMatrix[1][1] *= -1.0f;

                    camera.IsDirty = false;
                }

                // Calculate View Matrix (Inverse of World Transform)
                camera.ViewMatrix = Math::Inverse(transform.Matrix);
            }
        }
    };
}