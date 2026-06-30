// ReSTIR DI — final shading. Reads each pixel's resolved reservoir and writes the selected point light's
// contribution to the direct-lighting image. AsyncCompute.
//
// CONTRACT: this image stores DEMODULATED diffuse irradiance  E = Li * NdotL * W  (no albedo, no 1/PI).
// pbr.slang remodulates:  Lo += E * (albedo * (1-metallic) / PI). SVGF denoises E before remodulation.
// Keep this contract in lockstep with pbr_shade.slang's read. see arch/rendering-pipeline.md
import globals;
import restir_common;
import restir_di_target;
import brdf;                 // D_GGX / G_Smith (#154 specular shade)

[[vk::binding(0, 1)]] ByteAddressBuffer g_Lights;
[[vk::binding(0, 2)]] Sampler2D u_SceneDepth;
[[vk::binding(1, 2)]] Sampler2D u_SlimNormal;
// Reads the final reservoir at b6 (spatial-reuse output), not b2 — the temporal buffer stays untouched as
// next frame's history; the spatial pass writes the per-frame final result here.
[[vk::binding(6, 2)]] StructuredBuffer<Reservoir> reservoirs;
[[vk::binding(3, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDirectLighting;
[[vk::binding(7, 2)]] Sampler2D u_SlimRoughness;                                              // #154
[[vk::binding(8, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDirectSpec;      // #154 demodulated specular

struct PC
{
    float4x4 invViewProj;
    uint     candidateCount;
    uint     frameSeed;
    int      gbufferScale;
    int      dispatchW;
    int      dispatchH;
};
[[vk::push_constant]] PC pc;

PointLightData LoadPoint(uint j)
{
    uint b = 48u + j * 32u;
    PointLightData p;
    p.position  = asfloat(g_Lights.Load3(b + 0u));
    p.range     = asfloat(g_Lights.Load(b + 12u));
    p.color     = asfloat(g_Lights.Load3(b + 16u));
    p.intensity = asfloat(g_Lights.Load(b + 28u));
    return p;
}

int2 GbufCoord(int2 p)
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
    float3 S = float3(0.0);   // #154 demodulated specular (F0-free lobe)

    float depth = u_SceneDepth.Load(int3(GbufCoord(pixel), 0)).r;
    Reservoir r = reservoirs[flatIdx];
    if (depth < 1.0 && r.lightIndex != RESTIR_NO_LIGHT && r.W > 0.0)
    {
        float2 uv       = (float2(pixel) + 0.5) / float2(size);
        float4 worldH   = mul(pc.invViewProj, float4(uv * 2.0 - 1.0, depth, 1.0));
        float3 worldPos = worldH.xyz / worldH.w;
        float3 N        = OctDecode(u_SlimNormal.Load(int3(GbufCoord(pixel), 0)).rg);

        PointLightData pl = LoadPoint(r.lightIndex);
        float3 L; float dist;
        float3 Li = PointLightRadiance(pl, worldPos, L, dist);
        E = Li * max(dot(N, L), 0.0) * r.W;

        // #154 demodulated specular: S = Li·D·G/(4·NoV)·W (F0-free — pbr.slang applies the per-pixel F0).
        // The spec BRDF's NoL (D·F·G/(4·NoL·NoV)·NoL) cancels, leaving /(4·NoV). D/G via brdf.slang.
        float3 V     = normalize(ubo.cameraPos - worldPos);
        float  rough = u_SlimRoughness.Load(int3(GbufCoord(pixel), 0)).r;
        float3 H     = normalize(V + L);
        float  NoV   = max(dot(N, V), 1.0e-4);
        S = Li * (D_GGX(N, H, rough) * G_Smith(N, V, L, rough) / (4.0 * NoV)) * r.W;
    }

    outDirectLighting[pixel] = float4(E, 1.0);
    outDirectSpec[pixel]     = float4(S, 1.0);
}
