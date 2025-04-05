#pragma once

#include "luth/scene/Components.h"

#include <entt/entt.hpp>

namespace Luth
{
    class System
    {
    public:
        virtual ~System() = default;
        virtual void Update(entt::registry& registry) = 0;
    };

    class RenderingSystem : public System
    {
    public:
        void Update(entt::registry& registry) override {
            auto view = registry.view<Transform, MeshRenderer>();

            view.each([this](Transform& transform, MeshRenderer& meshRend) {
                if (false/*!mesh.visible*/) return;

                // Rendering logic here
                const glm::mat4 model = CalculateModelMatrix(transform);
                // renderer->DrawMesh(mesh.meshId, model, mesh.color);

                // Actual rendering would happen here
                // Example pseudo-code:
                // shader->SetUniform("model", CalculateModelMatrix(transform));
                // renderer->DrawMesh(mesh.meshId);
            });
        }

    private:
        glm::mat4 CalculateModelMatrix(const Transform& t) {
            // TODO: Create a matrix from transform components
            return glm::mat4(1.0f);
        }
    };
}
