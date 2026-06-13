// GlobalUniforms (Set 0 binding 0). Twin of common/globals.glsl + the C++ RenderingSystem.h struct —
// the std140 layout must stay byte-identical across all three; MaterialLayoutGuard cross-checks at init.
// see arch/rendering-pipeline.md
public struct GlobalUniforms
{
    public float4x4 viewProjection;
    public float4x4 prevViewProjection;
    public float4x4 view;
    public float4x4 projection;
    public float3   cameraPos;
    public float    time;
    public float4x4 lightSpaceMatrix[4];
    public float4   cascadeSplitsViewZ;
    public float4   shadowBias;
    public float4   shadowNormalBias;
    public float4   cascadeTexelSize;
    public float    iblIntensity;
    public float    skyboxIntensity;
    public float    debugVisualizeCascades;
    public float    cascadeBlendWidth;
    public float2   viewportSize;
    public float    nearZ;
    public float    farZ;
    public float4   distanceFogColorDensity;   // rgb = color, a = density
    public float4   distanceFogParams;         // x = start, y = maxOpacity, z = enabled
    public float4   heightFogColorDensity;
    public float4   heightFogParams;           // x = refHeight, y = falloff, z = enabled, w = multiScatter
    public float4   volTemporalParams;         // x = anisotropy, y = temporalAlpha, z = sunFogAbsorptionSteps, w = skyFogStrength
    public float4   prevViewParams;            // x = prevNearZ, y = prevFarZ
    public float4   volNoiseParams;            // x = noiseScale, y = noiseStrength
    public float4   volNoiseWind;              // xyz = wind dir × speed (m/s)
    public float4   volScatterParams;          // x = scatteringIntensity, y = blueNoiseDither
    public float4   specAaParams;              // x = specularAaEnabled, y = specularAaSigma (Tokuyoshi 2019)
    public float4   taaParams;                 // x = taaEnabled, y = temporalAlpha, zw = currentJitter (px)
    public float4   prevJitter;                // xy = prevJitter (px) — source-side de-jitter
    public float4   rtShadowParams;            // x = shadowingMode (0=CSM, 1=RT), y = originEps, z = normalEps
    public float4   restirParams;              // x = ReSTIR DI enabled, y = ReSTIR GI enabled, z = DI-specular
    public float4   pathTraceParams;           // x = enabled, y = samplesPerFrame, z = maxBounces, w = accumCount
    public float4   reflParams;                // x = RT reflections enabled, y = roughFadeStart, z = roughFadeEnd
    public float4x4 invViewProjection;         // depth → world. APPENDED — never insert mid-struct (inline-prefix shaders desync)
}

[[vk::binding(0, 0)]] public ConstantBuffer<GlobalUniforms> ubo;
