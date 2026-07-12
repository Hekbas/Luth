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
        StaticSwitch,   // compile-time select: emits Off (slot 0) or On (slot 1) per value.x; flips recompile
        Subtract,       // in0 - in1
        Divide,         // in0 / in1 (raw; inf/NaN visible, matches Unity)
        Power,          // pow(max(in0, 0), in1)
        Min,            // min(in0, in1)
        Max,            // max(in0, in1)
        Dot,            // float4(dot(in0.xyz, in1.xyz))
        Abs,            // abs(in0)
        Saturate,       // saturate(in0)
        OneMinus,       // 1 - in0
        UV,             // uv0 or uv1 (tex selects the set) as float4(uv, 0, 0)
        Noise,          // FBM value noise of a UV input (unlinked = uv0); value = (scale, octaves, -, -) as data
        WorldPos,       // fetch.WorldPos() as float4(p, 0)
        ViewDir,        // fetch.ViewDir() (toward camera; RT = -rayDir) as float4(v, 0)
        Time,           // fetch.Time() broadcast
        Fresnel,        // pow(1 - fetch.NdotV(), power); value.x = power as data; vertex-normal based
        Custom,         // user Slang float4 expression over inputs a..d (MatNode::code); sandbox-scanned
        Triplanar,      // world-projected 3-plane sample of map slot `tex`; tiling = value.x (data)
        DetailNormal,   // RNM detail-normal stack: in0 base tangent-normal, in1 detail; strength = value.x
        MakeLayer,      // 6 channel inputs -> a MaterialInputs "layer" bundle (Make Material Attributes)
        LayerBlend,     // per-channel mask blend of two layers: in0 bottom, in1 top, in2 float4 mask
        Parallax,       // parallax-occlusion: marches the height map along the tangent view dir -> displaced UV
        Decal           // UV-space decal: places an RGBA decal over a base layer, alpha-masked + box-confined
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

        // Custom node only: the emitted float4 expression over a..d. Enters the canonical source verbatim
        // (edits are structure edits); sandbox-scanned by MaterialGraphCodegen::ValidateCustomCode.
        std::string code;
    };

    // Node types whose value (or tex slot / switch state) is meaningful as an exposed material parameter.
    inline bool IsExposableNode(MatNodeType t)
    {
        return t == MatNodeType::ConstFloat || t == MatNodeType::ConstColor
            || t == MatNodeType::Remap      || t == MatNodeType::TextureSample
            || t == MatNodeType::StaticSwitch || t == MatNodeType::Noise
            || t == MatNodeType::Fresnel      || t == MatNodeType::Triplanar
            || t == MatNodeType::DetailNormal || t == MatNodeType::Parallax || t == MatNodeType::Decal;
    }

    // Nodes whose SSA local is a MaterialInputs "layer" bundle, not a float4. The codegen emits them as
    // MaterialInputs locals; the editor keeps their wires apart from the float4 value pins.
    inline bool IsLayerNode(MatNodeType t)
    {
        return t == MatNodeType::MakeLayer || t == MatNodeType::LayerBlend || t == MatNodeType::Decal;
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
