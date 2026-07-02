// Debug viz: heat-maps the ReSTIR GI spatial-reuse reservoir over LDR. M (confidence / merged sample
// count) drives the blue->green->yellow->red ramp; age dims the result, so a converged-but-stale region reads
// dim-red and a fresh-low-confidence region bright-blue. Empty reservoirs read near-black. Fullscreen
// triangle; the full-res overlay pixel maps to the GI reservoir cell (half-res when half-res GI is on).
import restir_gi_common;    // GIReservoir

[[vk::binding(0, 0)]] Sampler2D u_SceneDepth;
[[vk::binding(1, 0)]] StructuredBuffer<GIReservoir> reservoirs;

struct PC
{
    float2 viewportSize;    // full-res overlay extent
    float2 reservoirSize;   // GI reservoir extent (half when half-res GI)
    float  mCap;            // M heat-ramp saturates here
    float  ageCap;          // age fully dims here
};
[[vk::push_constant]] PC pc;

float3 HeatColor(float t)
{
    t = clamp(t, 0.0, 1.0);
    if (t < 0.33)      return lerp(float3(0.0, 0.2, 0.7), float3(0.0, 0.7, 0.2), smoothstep(0.0, 0.33, t));
    else if (t < 0.66) return lerp(float3(0.0, 0.7, 0.2), float3(0.95, 0.85, 0.1), smoothstep(0.33, 0.66, t));
    else               return lerp(float3(0.95, 0.85, 0.1), float3(0.95, 0.15, 0.05), smoothstep(0.66, 1.0, t));
}

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_TexCoord, float4 fragCoord : SV_Position) : SV_Target
{
    int2 px   = int2(fragCoord.xy);
    int2 size = int2(pc.viewportSize);
    if (px.x >= size.x || px.y >= size.y) return float4(0.0, 0.0, 0.0, 0.85);

    float depth = u_SceneDepth.Load(int3(px, 0)).r;
    if (depth >= 1.0) return float4(0.0, 0.0, 0.0, 0.85);   // sky -> dark overlay

    // Map the full-res overlay pixel to the GI reservoir cell (half-res when half-res GI is on).
    int2 res = int2(pc.reservoirSize);
    int2 rp  = clamp(int2((float2(px) + 0.5) / pc.viewportSize * pc.reservoirSize), int2(0), res - int2(1));
    uint flatIdx = uint(rp.y) * uint(res.x) + uint(rp.x);
    GIReservoir r = reservoirs[flatIdx];

    float3 col;
    if (r.M == 0u)
    {
        col = float3(0.04);                                   // empty reservoir -> near-black
    }
    else
    {
        float mNorm   = (pc.mCap   > 0.0) ? float(r.M)   / pc.mCap   : 0.0;
        float ageNorm = (pc.ageCap > 0.0) ? float(r.age) / pc.ageCap : 0.0;
        float bright  = lerp(1.0, 0.2, clamp(ageNorm, 0.0, 1.0));   // stale -> dim
        col = HeatColor(mNorm) * bright;
    }
    return float4(col, 0.85);
}
