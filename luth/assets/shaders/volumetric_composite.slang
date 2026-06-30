// Volumetric fog composite. Samples the integrated + resolved atlas at the fragment's view-Z and emits a
// (fogColor, fogOpacity) pair for the engine's standard alpha-blend (src*src.a + dst*(1-src.a)) —
// equivalent to Beer-Lambert final = sceneColor*volTransmit + scatter. see arch/rendering-pipeline.md
import globals;
import froxel;

[[vk::binding(0, 1)]] Sampler2D sceneDepth;
[[vk::binding(1, 1)]] Sampler3D volInScatter;
[[vk::binding(2, 1)]] Sampler2D blueNoise;   // 64² R8 NEAREST+REPEAT

struct PC { float4x4 invView; };   // reserved for future view-space sampling; unused today
[[vk::push_constant]] PC pc;

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_TexCoord, float4 fragCoord : SV_Position) : SV_Target
{
    float ndcDepth = sceneDepth.Sample(v_TexCoord).r;
    float viewZ    = FroxelDepthToViewZ(ndcDepth, ubo.nearZ, ubo.farZ);
    bool  isSky    = ndcDepth >= 0.9999;

    // Blue-noise jitter on sliceW breaks Wronski's log-slice Z-banding by ±0.5 slices (TAA integrates it).
    float sliceW = FroxelViewZToSlice(viewZ, ubo.nearZ, ubo.farZ);
    if (ubo.volScatterParams.y != 0.0)
    {
        uint vw, vh, vd; volInScatter.GetDimensions(vw, vh, vd);
        float dither = blueNoise.Sample(fragCoord.xy / 64.0).r;
        sliceW += (dither - 0.5) * (1.0 / float(vd));
        sliceW  = clamp(sliceW, 0.0, 1.0);
    }
    float4 volS = volInScatter.Sample(float3(v_TexCoord, sliceW));
    float3 volScatter  = volS.rgb;
    float  volTransmit = volS.a;

    float dfMaxOpa = ubo.distanceFogParams.y;   // distance-fog max-opacity cap

    float fogOpacity = clamp(1.0 - volTransmit, 0.0, max(dfMaxOpa, 0.001));
    // Sky-pixel fog strength cap — keeps the skybox visible in dense fog.
    if (isSky)
        fogOpacity *= ubo.volTemporalParams.w;

    float3 fogColor = (fogOpacity > 1e-5) ? volScatter / fogOpacity : float3(0.0);
    return float4(fogColor, fogOpacity);
}
