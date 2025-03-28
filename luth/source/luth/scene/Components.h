#pragma once

#include "luth/core/Math.h"
#include "luth/scene/Entity.h"
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
        glm::vec3 m_Scale = { 1.0f, 1.0f, 1.0f };

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
}

namespace Luth { using namespace Luth::Component; }
