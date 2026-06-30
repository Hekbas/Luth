// Blue-noise-like dither bake — Martin Roberts' R2 plastic-number quasi-random sequence (low-discrepancy
// 2D stippling with a blue-noise-like spectrum). One-shot bake; sampler is NEAREST+REPEAT. Standalone Set 0.
[[vk::binding(0, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> u_BlueNoiseOut;

struct PC { uint dim; };   // 64 (texture side)
[[vk::push_constant]] PC pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 p = tid.xy;
    if (any(p >= uint2(pc.dim, pc.dim))) return;

    // R2 sequence: g = plastic number. Per-pixel = frac(0.5 + dot(coord, (1/g, 1/g²))). The irrational
    // coefficients kill 2D periodicity; frac wraps cleanly so REPEAT sampling is seamless.
    const float g = 1.32471795724474602596;
    const float2 a = float2(1.0 / g, 1.0 / (g * g));
    float value = frac(0.5 + dot(float2(p), a));

    u_BlueNoiseOut[int2(p)] = value;
}
