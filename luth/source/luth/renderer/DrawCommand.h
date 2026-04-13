#pragma once

#include "luth/core/LuthTypes.h"
#include "luth/renderer/Material.h"
#include "luth/renderer/Model.h"

#include <glm/glm.hpp>
#include <memory>

namespace Luth
{
    struct DrawCommand {
        glm::mat4 modelMatrix;
        u32 materialSlot;
        std::shared_ptr<Model> model;
        u32 meshIndex;
        u32 entityIndex = 0;
        Material::CullMode cullMode = Material::CullMode::Back;
        bool isSkinned = false;
        u32 boneOffset = 0;
    };

    struct ObjectPushConstants {
        glm::mat4 modelMatrix;  // 64 bytes
        u32 materialIndex;      // 4 bytes — index into material SSBO
        u32 shadeMode;          // 4 bytes
        u32 entityID;           // 4 bytes — entity index for picking
        u32 boneOffset;         // 4 bytes — base index into BoneMatrices SSBO (0 for static)
    };
}
