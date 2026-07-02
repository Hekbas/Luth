#pragma once

#include "luth/core/types/LuthMath.h"

#include <vector>

namespace Luth
{
    // Channel-routing node vocabulary for the material graph. Each type lowers to one derivative-free
    // Slang SSA expression in MaterialGraphCodegen, so the emitted EvalGraph<F> stays two-tier-safe (no
    // ddx / screen-space ops, valid in both the raster and ray-hit tiers). Authoring data only; the
    // graph is persisted on Material and lowered to a generated fragment shader at codegen time.
    enum class MatNodeType : u8
    {
        ConstFloat,     // value.x
        ConstColor,     // value.rgba
        TextureSample,  // samples material map slot `tex`; outputs float4
        Multiply,       // in0 * in1 (componentwise)
        Add,            // in0 + in1
        Lerp,           // lerp(in0, in1, in2)
        Remap,          // value = (inMin, inMax, outMin, outMax)
        Split,          // float4 in -> 4 scalar outs (.x .y .z .w)
        Output          // terminal: the 6 MaterialInputs channels
    };

    struct MatNode
    {
        u32         id    = 0;              // unique within the graph (editor-assigned)
        MatNodeType type  = MatNodeType::ConstFloat;
        Vec4        value = Vec4(0.0f);     // interpreted per type (see enum)
        u32         tex   = 0;              // TextureSample: MapType slot (Diffuse=0, Normal=2, ...)
        Vec2        pos   = Vec2(0.0f);     // editor canvas position
    };

    struct MatLink
    {
        u32 fromNode = 0;  u8 fromSlot = 0;   // producer node + its output slot
        u32 toNode   = 0;  u8 toSlot   = 0;   // consumer node + its input slot
    };

    struct MaterialGraph
    {
        std::vector<MatNode> nodes;
        std::vector<MatLink> links;

        bool Empty() const { return nodes.empty(); }
    };
}
