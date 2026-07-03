#pragma once

#include "luth/core/types/LuthMath.h"

#include <string>
#include <vector>

namespace Luth
{
    // Channel-routing node vocabulary for the material graph. Each type lowers to one derivative-free
    // Slang SSA expression in MaterialGraphCodegen, so the emitted EvalGraph<F> stays two-tier-safe (no
    // ddx / screen-space ops, valid in both the raster and ray-hit tiers). Authoring data only; the
    // graph is persisted on Material and lowered to a generated fragment shader at codegen time.
    // invariant: enum order is .mat serialization ABI (type persists as an int) - append, never reorder.
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
        Output,         // terminal: the 6 MaterialInputs channels
        StaticSwitch    // compile-time select: emits Off (slot 0) or On (slot 1) per value.x; flips recompile
    };

    struct MatNode
    {
        u32         id    = 0;              // unique within the graph (editor-assigned)
        MatNodeType type  = MatNodeType::ConstFloat;
        Vec4        value = Vec4(0.0f);     // interpreted per type (see enum)
        u32         tex   = 0;              // TextureSample: MapType slot (Diffuse=0, Normal=2, ...)
        Vec2        pos   = Vec2(0.0f);     // editor canvas position

        // Exposed-parameter metadata. Authoring-only: never read by codegen, so naming a node cannot
        // change the canonical source or split structure-shared shaders/variants.
        std::string name;                   // inspector label; empty = unexposed
        std::string group;                  // inspector section; empty = ungrouped
        u8          ui = 0;                 // widget hint: 0 = per-type default, 1 = checkbox (ConstFloat 0/1)
    };

    // Node types whose value (or tex slot / switch state) is meaningful as an exposed material parameter.
    inline bool IsExposableNode(MatNodeType t)
    {
        return t == MatNodeType::ConstFloat || t == MatNodeType::ConstColor
            || t == MatNodeType::Remap      || t == MatNodeType::TextureSample
            || t == MatNodeType::StaticSwitch;
    }

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
