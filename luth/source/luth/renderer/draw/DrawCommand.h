#pragma once

#include "luth/core/types/LuthMath.h"
#include "luth/renderer/material/Material.h"
#include "luth/renderer/resources/Model.h"

#include <entt/entt.hpp>
#include <memory>

namespace Luth
{
    // Per-draw data structures used across the renderer. DrawCommand is the CPU-side entry
    // assembled by DrawListBuilder. ObjectPushConstants packs the small per-draw values pushed
    // at each Bind. GPUObjectData is the SSBO layout (std430, 112B) consumed by gpu_cull.comp
    // and the PBR vertex shader, so its layout must stay in lockstep with the GLSL counterpart.
    struct DrawCommand {
        Mat4 modelMatrix;
        u32 materialSlot;
        std::shared_ptr<Model> model;
        u32 meshIndex;
        u32 entityIndex = 0;
        entt::entity entity = entt::null;
        Material::CullMode cullMode = Material::CullMode::Back;
        bool isSkinned = false;
        u32 boneOffset = 0;
        u32 gpuObjectIndex = 0;  // 0-based index into GPUObjectData SSBO / IndirectBuffer
    };

    struct ObjectPushConstants {
        Mat4 modelMatrix;  // 64 bytes
        u32 materialIndex;      // 4 bytes — index into material SSBO
        u32 shadeMode;          // 4 bytes
        u32 entityID;           // 4 bytes — entity index for picking
        u32 boneOffset;         // 4 bytes — base index into BoneMatrices SSBO (0 for static)
    };

    // Per-object data uploaded to GPU SSBO each frame (std430 layout, 176 bytes)
    struct GPUObjectData {
        Mat4 model;          // 64B
        Mat4 prevModel;      // 64B — frame N-1's worldMatrix (motion vectors)
        Vec4 boundingSphere; // 16B — xyz=center (local space), w=radius (local space)
        u32 materialIndex;        // 4B
        u32 shadeMode;            // 4B
        u32 entityID;             // 4B
        u32 boneOffset;           // 4B
        u32 indexCount;           // 4B
        u32 firstIndex;           // 4B
        i32 vertexOffset;         // 4B
        u32 prevBoneOffset;       // 4B — offset into BoneMatrixBuffer's prev-bones region (commit 2 wires it; 0 until then)
    };
    static_assert(sizeof(GPUObjectData) == 176, "GPUObjectData std430 layout must stay in lockstep with the GLSL block");
}
