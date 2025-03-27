#pragma once

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
}

namespace Luth { using namespace Luth::Component; }
