// ReSTIR GI final shading. Reads each pixel's resolved GI reservoir and writes the demodulated
// indirect-diffuse irradiance. AsyncCompute.
//
// CONTRACT: stores DEMODULATED indirect irradiance  E = L_o * NdotL_v * W  (no primary albedo, no 1/PI).
// pbr.slang remodulates:  Lo += E * (albedo * (1-metallic) / PI). SVGF denoises E before remodulation,
// same contract as the DI path. L_o already contains the secondary surface's albedo + cosine; only the
// PRIMARY-vertex cosine (NdotL_v) and W are applied here. Reads the spatial-reuse output at b6.
// see arch/rendering-pipeline.md
import globals;
import restir_common;        // OctDecode
import restir_gi_common;     // GIReservoir

[[vk::binding(0, 2)]] Sampler2D u_SceneDepth;
[[vk::binding(1, 2)]] Sampler2D u_SlimNormal;
[[vk::binding(6, 2)]] StructuredBuffer<GIReservoir> reservoirs;
[[vk::binding(3, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outIndirect;

struct PC
{
    float4x4 invViewProj;
    uint     frameSeed;
    float    secondaryAlbedo;
    float    maxIndirect;
    int      gbufferScale;
    int      dispatchW;
    int      dispatchH;
};
[[vk::push_constant]] PC pc;

int2 GiGuide(int2 p)
{
    if (pc.gbufferScale == 1) return p;
    float2 uv   = (float2(p) + 0.5) / float2(pc.dispatchW, pc.dispatchH);
    int2   full = int2(ubo.viewportSize);
    return clamp(int2(uv * float2(full)), int2(0), full - int2(1));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2 pixel = int2(tid.xy);
    int2 size  = int2(pc.dispatchW, pc.dispatchH);
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    uint flatIdx = uint(pixel.y) * uint(size.x) + uint(pixel.x);

    float3 E = float3(0.0);

    float depth = u_SceneDepth.Load(int3(GiGuide(pixel), 0)).r;
    GIReservoir r = reservoirs[flatIdx];
    if (depth < 1.0 && r.M > 0u && r.W > 0.0)
    {
        float2 uv       = (float2(pixel) + 0.5) / float2(size);
        float4 worldH   = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
        float3 worldPos = worldH.xyz / worldH.w;
        float3 Nv       = OctDecode(u_SlimNormal.Load(int3(GiGuide(pixel), 0)).rg);

        float3 L  = r.samplePos - worldPos;
        float  d2 = dot(L, L);
        if (d2 > 0.0)
        {
            L *= rsqrt(d2);
            E = r.sampleRadiance * max(dot(Nv, L), 0.0) * r.W;
        }
    }

    if (any(isnan(E)) || any(isinf(E))) E = float3(0.0);   // never poison SVGF / remodulation
    outIndirect[pixel] = float4(E, 1.0);
}
