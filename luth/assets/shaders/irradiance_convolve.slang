// Diffuse irradiance convolution — cosine-weighted hemisphere integral of the env cubemap per face texel.
// Standalone Set 0.
[[vk::binding(0, 0)]] SamplerCube envMap;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> irradianceOut;

static const float PI = 3.14159265359;

float3 GetCubemapDirection(uint face, float2 uv)
{
    float2 st = uv * 2.0 - 1.0;
    switch (face)
    {
        case 0: return normalize(float3( 1.0, -st.y, -st.x));
        case 1: return normalize(float3(-1.0, -st.y,  st.x));
        case 2: return normalize(float3( st.x,  1.0,  st.y));
        case 3: return normalize(float3( st.x, -1.0, -st.y));
        case 4: return normalize(float3( st.x, -st.y,  1.0));
        case 5: return normalize(float3(-st.x, -st.y, -1.0));
    }
    return float3(0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int3 texCoord = int3(tid);
    uint w, h, layers; irradianceOut.GetDimensions(w, h, layers);
    if (texCoord.x >= int(w) || texCoord.y >= int(h) || texCoord.z >= 6) return;

    float2 uv = (float2(texCoord.xy) + 0.5) / float2(w, h);
    float3 N  = GetCubemapDirection(uint(texCoord.z), uv);

    float3 up    = abs(N.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
    float3 right = normalize(cross(up, N));
    up = cross(N, right);

    float3 irradiance = float3(0.0);
    float  sampleCount = 0.0;
    const float sampleDelta = 0.025;
    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            float3 tangentSample = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            irradiance += envMap.Sample(sampleVec).rgb * cos(theta) * sin(theta);
            sampleCount += 1.0;
        }
    }
    irradiance = PI * irradiance / sampleCount;

    irradianceOut[uint3(texCoord)] = float4(irradiance, 1.0);
}
