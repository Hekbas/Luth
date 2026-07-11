// ReSTIR DI final shading. Reads each pixel's resolved reservoir and writes the selected point light's
// contribution to the direct-lighting image. AsyncCompute.
//
// CONTRACT: this image stores DEMODULATED diffuse irradiance  E = Li * NdotL * W  (no albedo, no 1/PI).
// pbr.slang remodulates:  Lo += E * (albedo * (1-metallic) / PI). SVGF denoises E before remodulation.
// The spec output additionally divides out DiSpecRho (Karis envBRDF, F0=1); pbr_shade multiplies it back.
// Both outputs' ALPHA carries reservoir confidence saturate(M / confidenceNorm) for the SVGF reproject.
// Keep this contract in lockstep with pbr_shade.slang's read. see arch/rendering-pipeline.md
import globals;
import restir_common;
import restir_di_target;
import brdf;                 // D_GGX / G_Smith (specular shade)

[[vk::binding(0, 1)]] ByteAddressBuffer g_Lights;
[[vk::binding(0, 2)]] Sampler2D u_SceneDepth;
[[vk::binding(1, 2)]] Sampler2D u_SlimNormal;
// Reads the final reservoir at b6 (spatial-reuse output, final visibility already folded into W), not b2;
// that same buffer then persists as next frame's temporal history.
[[vk::binding(6, 2)]] StructuredBuffer<Reservoir> reservoirs;
[[vk::binding(3, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDirectLighting;
[[vk::binding(7, 2)]] Sampler2D u_SlimRoughness;
[[vk::binding(8, 2)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDirectSpec;      // demodulated specular

struct PC
{
    float4x4 invViewProj;
    uint     candidateCount;
    uint     frameSeed;
    int      gbufferScale;
    int      dispatchW;
    int      dispatchH;
    float    diSpecClamp;    // luminance cap on the demodulated spec lobe (1/(4*NoV) blows up at grazing)
    uint2    _padGeomBDA;    // C++ RestirPC.geomTableBDA (initial pass only); keeps confidenceNorm at 96
    float    confidenceNorm; // reservoir M mapping to full denoiser confidence (output alpha)
};
[[vk::push_constant]] PC pc;

// Karis mobile EnvBRDFApprox with F0 = 1 (rho = A + B): the exactly-invertible scalar the demodulated
// spec lobe is divided by, so the denoised signal no longer bakes the roughness/view response (NRD input
// form). pbr_shade.slang remodulates with the same formula; keep the two in lockstep.
float DiSpecRho(float NoV, float rough)
{
    float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    float4 r  = rough * c0 + c1;
    float  a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return ab.x + ab.y;
}

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
    float3 S = float3(0.0);   // demodulated specular (F0-free lobe)

    float depth = u_SceneDepth.Load(int3(GbufCoord(pixel), 0)).r;
    Reservoir r = reservoirs[flatIdx];
    if (depth < 1.0 && r.lightIndex != RESTIR_NO_LIGHT && r.W > 0.0)
    {
        float2 uv       = (float2(pixel) + 0.5) / float2(size);
        float2 uvR      = DejitterReconUV(uv, ubo.taaParams.zw);   // match reservoir recon; see globals
        float4 worldH   = mul(pc.invViewProj, float4(uvR * 2.0 - 1.0, depth, 1.0));
        float3 worldPos = worldH.xyz / worldH.w;
        float3 N        = OctDecode(u_SlimNormal.Load(int3(GbufCoord(pixel), 0)).rg);

        PointLightData pl = LoadPoint(r.lightIndex);
        float3 L; float dist;
        float3 Li = PointLightRadiance(pl, worldPos, L, dist);
        E = Li * max(dot(N, L), 0.0) * r.W;

        // Demodulated specular: S = Li*D*G/(4*NoV)*W (F0-free; pbr.slang applies the per-pixel F0).
        // The spec BRDF's NoL (D*F*G/(4*NoL*NoV)*NoL) cancels, leaving /(4*NoV). D/G via brdf.slang.
        float3 V     = normalize(ubo.cameraPos - worldPos);
        float  rough = u_SlimRoughness.Load(int3(GbufCoord(pixel), 0)).r;
        float3 H     = normalize(V + L);
        float  NoV   = max(dot(N, V), 1.0e-4);
        S = Li * (D_GGX(N, H, rough) * G_Smith(N, V, L, rough) / (4.0 * NoV)) * r.W;

        // The spec lobe's 1/(4*NoV) reaches ~2500x at grazing; cap its luminance before the denoiser
        // smears the spike. Diffuse E is left to the boiling filter + final visibility.
        float slum = Luminance(S);
        if (slum > pc.diSpecClamp) S *= pc.diSpecClamp / slum;

        // Demodulate the roughness/view response AFTER the clamp (clamp semantics unchanged); the
        // denoiser filters the flat(ter) signal, pbr_shade.slang multiplies DiSpecRho back. With the
        // DiSpecular denoiser off the pair must be an exact no-op.
        S /= max(DiSpecRho(NoV, rough), 1.0e-2);
    }

    // Never poison SVGF / remodulation with a non-finite value (parity with the GI shade guard).
    if (any(isnan(E)) || any(isinf(E))) E = float3(0.0);
    if (any(isnan(S)) || any(isinf(S))) S = float3(0.0);

    // Alpha carries reservoir confidence for the motion-variant SVGF reproject's history-cap control
    // (empty / post-boil reservoirs re-converge fast). Both outputs share one reservoir -> one value.
    float conf = saturate(float(r.M) / max(pc.confidenceNorm, 1.0));
    outDirectLighting[pixel] = float4(E, conf);
    outDirectSpec[pixel]     = float4(S, conf);
}
