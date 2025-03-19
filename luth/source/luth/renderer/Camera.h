#pragma once

#include "luth/core/LuthTypes.h"

namespace Luth
{
    class Camera
    {
    public:
        Camera(float fov, float aspect, float nearClip, float farClip);

        void SetPosition(const Vec3& position) { m_Position = position; }
        void SetRotation(const Quat& rotation) { m_Rotation = rotation; }

        Mat4 GetViewMatrix() const;
        Mat4 GetProjectionMatrix() const { return m_Projection; }

        // For perspective control
        void SetPerspective(float fov, float aspect, float near, float far);

    private:
        Vec3 m_Position = { 0.0f, 0.0f, 5.0f };
        Quat m_Rotation = glm::identity<Quat>();
        Mat4 m_Projection;
    };
}
