// Volumetric fog inject — density pass. Per voxel: accumulate fog density + tint from all sources
// (FogVolume rows + analytic distance + analytic height), normalize tint as the density-weighted color
// average, then modulate density by 3D Worley-FBM noise. Writes vec4(density, tint.rgb) to volDensity.
// see arch/rendering-pipeline.md
import globals;

[[vk::binding(0, 1)]] [[vk::image_format("rgba16f")]] RWTexture3D<float4> volDensity;
// FogVolumeBuffer = header { uint count; uint3 _pad } (16 B) then FogVolumeRow[] (112 B std430 each) at
// offset 16. Read via raw byte offsets — a header + flexible array can't be a StructuredBuffer.
[[vk::binding(1, 1)]] ByteAddressBuffer g_Fog;
[[vk::binding(2, 1)]] Sampler3D volNoise3D;

struct PC
{
    float4x4 invView;
    uint     volDimX, volDimY, volDimZ, _pad;
};
[[vk::push_constant]] PC pc;

float SliceToViewZ(int i, float nearZ, float farZ)
{
    return nearZ * pow(farZ / nearZ, (float(i) + 0.5) / float(pc.volDimZ));
}

[shader("compute")]
[numthreads(8, 8, 4)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int3 volDim = int3(int(pc.volDimX), int(pc.volDimY), int(pc.volDimZ));
    int3 voxel  = int3(tid);
    if (any(voxel >= volDim)) return;

    // Voxel center → world via inverse-projection + invView push.
    float2 uv    = (float2(voxel.xy) + 0.5) / float2(volDim.xy);
    float2 ndcXY = uv * 2.0 - 1.0;
    float  viewZ = SliceToViewZ(voxel.z, ubo.nearZ, ubo.farZ);
    float  vsZ   = -viewZ;   // RH, -Z forward
    float  vsX   = ndcXY.x * (-vsZ) / ubo.projection[0][0];
    float  vsY   = ndcXY.y * (-vsZ) / ubo.projection[1][1];
    float3 worldPos = mul(pc.invView, float4(float3(vsX, vsY, vsZ), 1.0)).xyz;

    float  density    = 0.0;
    float3 colorAccum = float3(0.0);

    // 1. Local FogVolume rows (header at 0; rows at 16 + i*112). Fields used: worldToVolume, extents,
    //    type, color, density, falloffStart, falloffEnd (affectsAmbient/_pad unused here).
    uint count = g_Fog.Load(0u);
    for (uint i = 0u; i < count; ++i)
    {
        uint b = 16u + i * 112u;
        // worldToVolume — 4 column-major glm columns → transpose the row-ctor to columns.
        float4 c0 = asfloat(g_Fog.Load4(b +  0u));
        float4 c1 = asfloat(g_Fog.Load4(b + 16u));
        float4 c2 = asfloat(g_Fog.Load4(b + 32u));
        float4 c3 = asfloat(g_Fog.Load4(b + 48u));
        float4x4 worldToVolume = transpose(float4x4(c0, c1, c2, c3));
        float3 extents = asfloat(g_Fog.Load3(b + 64u));
        uint   type    = g_Fog.Load(b + 76u);
        float3 vColor  = asfloat(g_Fog.Load3(b + 80u));
        float  vDens   = asfloat(g_Fog.Load(b + 92u));
        float  fStart  = asfloat(g_Fog.Load(b + 96u));
        float  fEnd    = asfloat(g_Fog.Load(b + 100u));

        float3 localPos = mul(worldToVolume, float4(worldPos, 1.0)).xyz;
        float d = 0.0;
        bool inside = false;
        if (type == 0u)
        {
            float3 e = max(extents, float3(1e-4));
            float3 r = abs(localPos) / e;
            d = max(max(r.x, r.y), r.z);
            inside = d <= 1.0;
        }
        else
        {
            float r = max(extents.x, 1e-4);
            d = length(localPos) / r;
            inside = d <= 1.0;
        }
        if (!inside) continue;
        float fEndC   = max(fEnd, 1e-4);
        float fStartC = clamp(fStart, 0.0, fEndC);
        float fall    = clamp((fEndC - d) / max(fEndC - fStartC, 1e-4), 0.0, 1.0);
        float dContrib = vDens * fall;
        density    += dContrib;
        colorAccum += vColor * dContrib;
    }

    // 2. Analytic distance fog — uniform density past dfStart.
    float  dfDensityRaw = ubo.distanceFogColorDensity.a;
    float  dfEnabled    = ubo.distanceFogParams.z;
    float  dfStart      = ubo.distanceFogParams.x;
    float3 dfColor      = ubo.distanceFogColorDensity.rgb;
    float  camDistVoxel = length(worldPos - ubo.cameraPos);
    float  dfDensity_v  = ((camDistVoxel > dfStart) ? dfDensityRaw : 0.0) * dfEnabled;
    density    += dfDensity_v;
    colorAccum += dfColor * dfDensity_v;

    // 3. Analytic height fog — exponential profile below the reference height.
    float  hfDensityRaw = ubo.heightFogColorDensity.a;
    float  hfEnabled    = ubo.heightFogParams.z;
    float  hfRefH       = ubo.heightFogParams.x;
    float  hfFalloff    = ubo.heightFogParams.y;
    float3 hfColor      = ubo.heightFogColorDensity.rgb;
    float  depthBelow   = max(hfRefH - worldPos.y, 0.0);
    float  hfDensity_v  = hfDensityRaw * (1.0 - exp(-depthBelow * hfFalloff)) * hfEnabled;
    density    += hfDensity_v;
    colorAccum += hfColor * hfDensity_v;

    density = max(density, 0.0);
    float3 tint = (density > 1e-4) ? colorAccum / density : float3(1.0);

    // 3D Worley-FBM noise modulates density in world space (REPEAT sampler tiles the 128³ atlas). Tint is
    // captured BEFORE noise so colour stays stable.
    float noiseStrength = ubo.volNoiseParams.y;
    if (noiseStrength > 1e-4 && density > 1e-4)
    {
        float3 noiseUVW = worldPos * ubo.volNoiseParams.x + ubo.volNoiseWind.xyz * ubo.time * ubo.volNoiseParams.x;
        float  n        = volNoise3D.Sample(noiseUVW).a;
        density *= lerp(1.0 - noiseStrength, 1.0 + noiseStrength, n);
    }

    // .r = density (integrate Beer-Lambert + viz); .gba = tint (scatter per-voxel colour modulation).
    volDensity[voxel] = float4(density, tint.r, tint.g, tint.b);
}
