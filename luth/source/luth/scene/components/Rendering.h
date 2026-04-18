#pragma once

#include "luth/core/UUID.h"

#include <cstdint>
#include <string>

namespace Luth::Component
{
    struct MeshRenderer {
        UUID ModelUUID;
        uint32_t MeshIndex = 0;
        UUID MaterialUUID;
        bool isSkinned;

        // Tmp state for ImGui
        std::string modelNamePreview;
        std::string materialNamePreview;
    };
}
