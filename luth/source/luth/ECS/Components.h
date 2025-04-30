#pragma once

#include "luth/core/Math.h"
#include "luth/ECS/Entity.h"
#include "luth/core/UUID.h"

#include <entt/entt.hpp>
#include <string>
#include <vector>

namespace Luth::Component
{
    struct ID {
        UUID m_ID;

        ID() = default;
        ID(const ID&) = default;
    };

    struct Tag {
        std::string m_Tag;

        Tag() = default;
        Tag(const Tag&) = default;
        Tag(const std::string& tag) : m_Tag(tag) {}
    };

    struct Parent {
        Entity m_Parent;

        Parent() = default;
        Parent(const Parent&) = default;
        Parent(const Entity& parent) : m_Parent(parent) {}
    };

    struct Children {
        std::vector<Entity> m_Children;

        Children() = default;
        Children(const Children&) = default;
        Children(const std::vector<Entity>& children) : m_Children(children) {}
    };

    struct Transform {
        glm::vec3 m_Position = { 0.0f, 0.0f, 0.0f };
        glm::vec3 m_Rotation = { 0.0f, 0.0f, 0.0f }; // Euler angles (degrees)
        glm::vec3 m_Scale    = { 1.0f, 1.0f, 1.0f };

        glm::mat4 GetTransform() const {
            glm::mat4 rotation = glm::toMat4(
                glm::quat(glm::radians(m_Rotation))
            );

            return glm::translate(glm::mat4(1.0f), m_Position)
                * rotation
                * glm::scale(glm::mat4(1.0f), m_Scale);
        }

        /*glm::mat4 GetWorldTransform() const {
            auto& transform = GetComponent<Transform>();
            glm::mat4 result = transform.GetTransform();

            if (HasComponent<Parent>()) {
                Entity parent = GetComponent<Parent>().m_Parent;
                if (parent) {
                    result = parent.GetWorldTransform() * result;
                }
            }
            return result;
        }*/
    };

    struct Camera {
        enum class ProjectionType { Perspective = 0, Orthographic = 1 };

        ProjectionType Projection = ProjectionType::Perspective;

        // Perspective properties
        float VerticalFOV = 45.0f;
        float NearClip = 0.01f;
        float FarClip = 1000.0f;

        // Orthographic properties
        float OrthographicSize = 10.0f;
        float OrthographicNear = -1.0f;
        float OrthographicFar = 1.0f;

        float AspectRatio = 16.0f / 9.0f;

        glm::mat4 ViewMatrix;
        glm::mat4 ProjectionMatrix;

        Camera() = default;
        Camera(const Camera&) = default;

        void SetPerspective(float verticalFOV, float nearClip, float farClip) {
            Projection = ProjectionType::Perspective;
            VerticalFOV = verticalFOV;
            NearClip = nearClip;
            FarClip = farClip;
            RecalculateProjection();
        }

        void SetOrthographic(float size, float nearClip, float farClip) {
            Projection = ProjectionType::Orthographic;
            OrthographicSize = size;
            OrthographicNear = nearClip;
            OrthographicFar = farClip;
            RecalculateProjection();
        }

        void RecalculateProjection() {
            if (Projection == ProjectionType::Perspective) {
                ProjectionMatrix = glm::perspective(
                    glm::radians(VerticalFOV),
                    AspectRatio,
                    NearClip,
                    FarClip
                );
            }
            else {
                float orthoLeft = -OrthographicSize * AspectRatio * 0.5f;
                float orthoRight = OrthographicSize * AspectRatio * 0.5f;
                float orthoBottom = -OrthographicSize * 0.5f;
                float orthoTop = OrthographicSize * 0.5f;

                ProjectionMatrix = glm::ortho(
                    orthoLeft, orthoRight,
                    orthoBottom, orthoTop,
                    OrthographicNear,
                    OrthographicFar
                );
            }
        }

        glm::mat4 GetViewProjection(const glm::mat4& transform) const {
            return ProjectionMatrix * glm::inverse(transform);
        }
    };

    struct MeshRenderer {
        UUID ModelUUID;
        uint32_t MeshIndex = 0;
        UUID MaterialUUID;

        // Tmp state for ImGui
        std::string modelNamePreview;
        std::string materialNamePreview;
    };

    struct DirectionalLight {
        Vec3 Color = Vec3(1.0f);
        float Intensity = 1.0f;
    };
    
    struct PointLight {
        Vec3 Color = Vec3(1.0f);;
        float Intensity = 1.0f;
        float Range = 350.0f;
    };
}

namespace Luth { using namespace Luth::Component; }
