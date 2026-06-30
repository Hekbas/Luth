// Per-vertex compute deformation for STATIC meshes opted into the deformation seam. Reads the source
// Vertex VB via a raw BDA float* (scalar/tight 52 B = 13 floats), applies procedural wind — main bend +
// gust envelope + detail/turbulence — and writes the post-deform vertex (interleaved Vertex, 13 floats)
// to the per-mesh deformed buffer the BLAS refit + geometry table + raster VS all consume — RT-correct by
// construction. Wind direction arrives in OBJECT space (CPU transforms the global world-space dir per
// instance); per-entity strength/phase ride the push constant. Per-asset, so instances deform in lockstep
// (last-writer-wins for per-entity params); the per-vertex spatial phase gives intra-mesh motion.
//
// CONTRACT: input layout MUST match Model.h::Vertex (scalar/tight, 52 B / 13 floats): pos@0, normal@3,
// uv0@6, uv1@8, tangent@10 — locked by static_assert in VulkanAccelerationStructure.cpp.
//
// CONTRACT: deformed output MUST match the Vertex layout material.slang::GatherHitGeometry reads — 13
// floats/vert, byte-identical to skinning.slang's output. Normal/tangent PASS THROUGH (bind pose): the
// bend is low-frequency, shading error is second-order; recompute-from-bend-gradient is a future refinement.

struct PC
{
    float* inputBuf;     // raw float view of Vertex[] (13 floats/vert), scalar/tight
    float* deformedBuf;  // interleaved Vertex out (13 floats/vert)
    uint   vertexCount;
    float  time;
    float  windX, windY, windZ;
    float  strength;
    float  mainBendScale;
    float  detailScale;
    float  frequency;
    float  phaseOffset;
    float  gustStrength;
    float  gustFrequency;
    float  turbAmplitude;
    float  turbFrequency;
};
[[vk::push_constant]] PC pc;

// Smoothed triangle wave (Crysis, GPU Gems 3) — cheaper than sin, visually similar. Output in [0,1].
float4 smoothTriangleWave(float4 x)
{
    float4 tri = abs(frac(x + 0.5) * 2.0 - 1.0);
    return tri * tri * (3.0 - 2.0 * tri);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint vid = tid.x;
    if (vid >= pc.vertexCount) return;

    const uint inB = vid * 13u;
    float3 position = float3(pc.inputBuf[inB +  0u], pc.inputBuf[inB +  1u], pc.inputBuf[inB +  2u]);
    float3 normal   = float3(pc.inputBuf[inB +  3u], pc.inputBuf[inB +  4u], pc.inputBuf[inB +  5u]);
    float2 uv0      = float2(pc.inputBuf[inB +  6u], pc.inputBuf[inB +  7u]);
    float2 uv1      = float2(pc.inputBuf[inB +  8u], pc.inputBuf[inB +  9u]);
    float3 tangent  = float3(pc.inputBuf[inB + 10u], pc.inputBuf[inB + 11u], pc.inputBuf[inB + 12u]);

    const float3 windDir = float3(pc.windX, pc.windY, pc.windZ);   // object-space, CPU passes it normalized
    float3 p = position;

    // Spatial per-vertex phase → intra-mesh variation; phaseOffset de-syncs meshes sharing the field.
    const float  vtxPhase = dot(p, float3(0.91, 0.0, 0.43));
    const float4 phases   = float4(pc.time * pc.frequency + vtxPhase + pc.phaseOffset) + float4(0.0, 1.97, 0.79, 0.37);
    const float4 wave     = smoothTriangleWave(phases) * 2.0 - 1.0;   // [-1, 1]

    // Gust — low-frequency amplitude pulse modulating the main bend (never inverts the sway).
    const float gustRaw = smoothTriangleWave(float4(pc.time * pc.gustFrequency)).x;   // [0, 1]
    const float gust    = 1.0 + pc.gustStrength * (gustRaw * 2.0 - 1.0);

    // Main bend — sway along the wind direction, weighted by height above the local base (assumes +Y up).
    const float height   = max(p.y, 0.0);
    const float mainWave = (wave.x + wave.y) * 0.5;
    p += windDir * (pc.strength * pc.mainBendScale * height * mainWave * gust);

    // Detail bend — per-vertex shimmer along the normal, plus two hash-noise turbulence octaves.
    float detailWave = (wave.z + wave.w) * 0.5;
    const float t  = pc.time * pc.turbFrequency;
    const float n1 = smoothTriangleWave(float4(dot(p, float3(1.7, 2.3, 1.1)) + t)).x * 2.0 - 1.0;
    const float n2 = smoothTriangleWave(float4(dot(p, float3(3.1, 1.3, 2.9)) - t * 1.3)).x * 2.0 - 1.0;
    detailWave += pc.turbAmplitude * (n1 * 0.6 + n2 * 0.4);
    p += normal * (pc.strength * pc.detailScale * detailWave);

    const uint base = vid * 13u;            // 13 floats per interleaved Vertex
    pc.deformedBuf[base +  0u] = p.x;
    pc.deformedBuf[base +  1u] = p.y;
    pc.deformedBuf[base +  2u] = p.z;
    pc.deformedBuf[base +  3u] = normal.x;   // pass-through (bind pose) — see CONTRACT
    pc.deformedBuf[base +  4u] = normal.y;
    pc.deformedBuf[base +  5u] = normal.z;
    pc.deformedBuf[base +  6u] = uv0.x;
    pc.deformedBuf[base +  7u] = uv0.y;
    pc.deformedBuf[base +  8u] = uv1.x;
    pc.deformedBuf[base +  9u] = uv1.y;
    pc.deformedBuf[base + 10u] = tangent.x;
    pc.deformedBuf[base + 11u] = tangent.y;
    pc.deformedBuf[base + 12u] = tangent.z;
}
