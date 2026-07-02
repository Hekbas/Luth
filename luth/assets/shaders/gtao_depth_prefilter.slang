// GTAO depth prefilter: half-res view-space linear depth from a 2x2 full-res min-depth downsample
// (Jimenez 2016 / XeGTAO). Min biases AO away from background leaking through thin foreground silhouettes.
[[vk::binding(0, 0)]] Sampler2D u_SceneDepth;
[[vk::binding(1, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> u_LinearDepth;

struct PC
{
    int2  halfResSize;
    float2 invFullRes;
    float nearZ, farZ, _pad0, _pad1;
};
[[vk::push_constant]] PC pc;

float LinearizeDepth(float d) { return (pc.nearZ * pc.farZ) / (pc.farZ - d * (pc.farZ - pc.nearZ)); }

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 halfCoord = int2(tid.xy);
    if (halfCoord.x >= pc.halfResSize.x || halfCoord.y >= pc.halfResSize.y) return;

    float2 uv = (float2(halfCoord) + 0.5) * 2.0 * pc.invFullRes;
    float4 d4 = u_SceneDepth.GatherRed(uv);   // 4 texels around uv; order irrelevant for min

    float dMin = min(min(d4.x, d4.y), min(d4.z, d4.w));
    float linearZ = (dMin >= 1.0) ? pc.farZ : LinearizeDepth(dMin);
    u_LinearDepth[halfCoord] = linearZ;
}
