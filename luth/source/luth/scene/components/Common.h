#pragma once

#include "luth/core/UUID.h"
#include "luth/scene/Entity.h"

#include <string>
#include <vector>

namespace Luth::Component
{
    struct ID {
        UUID Value;

        ID() = default;
        ID(const ID&) = default;
    };

    struct Tag {
        std::string Value;

        Tag() = default;
        Tag(const Tag&) = default;
        Tag(const std::string& tag) : Value(tag) {}
    };

    struct Parent {
        Entity Value;

        Parent() = default;
        Parent(const Parent&) = default;
        Parent(const Entity& parent) : Value(parent) {}
    };

    struct Children {
        std::vector<Entity> Value;

        Children() = default;
        Children(const Children&) = default;
        Children(const std::vector<Entity>& children) : Value(children) {}
    };

    // Presence marks an inactive entity. Sparse by default (most entities are active).
    struct Disabled {};
}
