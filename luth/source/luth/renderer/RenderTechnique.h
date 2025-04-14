#pragma once

#include "luth/renderer/Framebuffer.h"
#include "luth/renderer/RenderPipeline.h"
#include "luth/ECS/Components.h"

#include <entt/entt.hpp>

namespace Luth
{
    struct RenderCommand {
        entt::entity entity;
        Transform* transform;
        MeshRenderer* meshRend;
        float distance;
    };

    class RenderTechnique
    {
    public:
        virtual ~RenderTechnique() = default;

        virtual void Init(u32 width, u32 height) = 0;
        virtual void Shutdown() = 0;

        virtual void Render(entt::registry& registry,
            const Vec3& cameraPos,
            const std::vector<RenderCommand>& opaque,
            const std::vector<RenderCommand>& transparent) = 0;
        virtual void Resize(u32 width, u32 height) = 0;

        virtual u32 GetFinalColorAttachment() const = 0;

        u32 GetWidth() const { return m_Width; }
        u32 GetHeight() const { return m_Height; }

    protected:
        u32 m_Width = 0;
        u32 m_Height = 0;
    };
}
