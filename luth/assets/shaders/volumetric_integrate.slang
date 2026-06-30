// Front-to-back ray march through the volumetric atlas. Each invocation walks one (x,y) froxel column over
// Z; accumulates per-step transmittance (Beer-Lambert) + in-scatter, written in-place into volInScatter:
// RGB = accumulated in-scatter, A = transmittance to the slice center (composition samples this).
// Standalone Set 0 (no global UBO — near/far ride the push constant).
//
// CONTRACT (Wronski 2014 / Hillaire 2015): inject_scatter writes scat = albedo × J; integrate accumulates
// transmit × scat × (1 − exp(−σ_t · dt)). Do NOT multiply by σ_t here. see arch/rendering-pipeline.md
[[vk::binding(0, 0)]] Sampler3D volDensity;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> volInScatter;

struct PC
{
    float4 nearFarPad;   // x = nearZ, y = farZ
    uint   volDimX, volDimY, volDimZ, _pad;
};
[[vk::push_constant]] PC pc;

float SliceToViewZ(uint i, float nearZ, float farZ)
{
    return nearZ * pow(farZ / nearZ, (float(i) + 0.5) / float(pc.volDimZ));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int3 volDim = int3(int(pc.volDimX), int(pc.volDimY), int(pc.volDimZ));
    int2 xy = int2(tid.xy);
    if (xy.x >= volDim.x || xy.y >= volDim.y) return;

    const float nearZ = pc.nearFarPad.x;
    const float farZ  = pc.nearFarPad.y;

    float3 accumRGB  = float3(0.0);
    float  transmit  = 1.0;
    float  prevViewZ = nearZ;

    for (int z = 0; z < volDim.z; ++z)
    {
        int3   voxel = int3(xy, z);
        float4 dens  = volDensity.Load(int4(voxel, 0));
        float4 scat  = volInScatter[voxel];

        float currViewZ = SliceToViewZ(uint(z), nearZ, farZ);
        float dt = max(currViewZ - prevViewZ, 0.0);
        prevViewZ = currViewZ;

        // Per-step Beer-Lambert. The stored alpha uses the HALF-step transmittance (value at slice center),
        // so composite's trilinear interp gets the right mid-slice transmittance at fragment depth.
        float extinction = dens.r;
        float stepT      = exp(-extinction * dt);
        accumRGB += transmit * scat.rgb * (1.0 - stepT);
        float halfStepT  = exp(-extinction * dt * 0.5);
        float transmitAtCenter = transmit * halfStepT;
        transmit *= stepT;

        volInScatter[voxel] = float4(accumRGB, transmitAtCenter);
    }
}
