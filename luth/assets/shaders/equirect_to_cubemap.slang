// Equirectangular HDR to cubemap face array. Standalone Set 0.
[[vk::binding(0, 0)]] Sampler2D equirectMap;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> cubemapOut;

static const float PI = 3.14159265359;

float3 GetCubemapDirection(uint face, float2 uv)
{
    float2 st = uv * 2.0 - 1.0;
    switch (face)
    {
        case 0: return normalize(float3( 1.0, -st.y, -st.x)); // +X
        case 1: return normalize(float3(-1.0, -st.y,  st.x)); // -X
        case 2: return normalize(float3( st.x,  1.0,  st.y)); // +Y
        case 3: return normalize(float3( st.x, -1.0, -st.y)); // -Y
        case 4: return normalize(float3( st.x, -st.y,  1.0)); // +Z
        case 5: return normalize(float3(-st.x, -st.y, -1.0)); // -Z
    }
    return float3(0.0);
}

float2 DirectionToEquirect(float3 dir)
{
    float phi   = atan2(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    return float2(phi / (2.0 * PI) + 0.5, theta / PI + 0.5);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int3 texCoord = int3(tid);
    uint w, h, layers; cubemapOut.GetDimensions(w, h, layers);
    if (texCoord.x >= int(w) || texCoord.y >= int(h) || texCoord.z >= 6) return;

    float2 uv  = (float2(texCoord.xy) + 0.5) / float2(w, h);
    float3 dir = GetCubemapDirection(uint(texCoord.z), uv);
    float2 equirectUV = DirectionToEquirect(dir);

    float3 color = equirectMap.Sample(equirectUV).rgb;
    cubemapOut[uint3(texCoord)] = float4(color, 1.0);
}
