// Pre-filtered env map (split-sum specular IBL, Karis 2013): GGX importance-sampled per roughness mip.
// Standalone Set 0; roughness arrives via push constant (one dispatch per mip).
[[vk::binding(0, 0)]] SamplerCube envMap;
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> prefilteredOut;

struct PC { float roughness; };
[[vk::push_constant]] PC pc;

static const float PI = 3.14159265359;

float RadicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
float2 Hammersley(uint i, uint N) { return float2(float(i) / float(N), RadicalInverse_VdC(i)); }

float3 ImportanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    float3 H = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float DistributionGGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

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
[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int3 texCoord = int3(tid);
    uint w, h, layers; prefilteredOut.GetDimensions(w, h, layers);
    if (texCoord.x >= int(w) || texCoord.y >= int(h) || texCoord.z >= 6) return;

    float2 uv = (float2(texCoord.xy) + 0.5) / float2(w, h);
    float3 N = GetCubemapDirection(uint(texCoord.z), uv);
    float3 R = N;
    float3 V = R;

    float  totalWeight = 0.0;
    float3 prefilteredColor = float3(0.0);
    const uint SAMPLE_COUNT = 1024u;

    // Source-mip selection from the GGX pdf (Krivanek/Colbert 2008): a sample covering a wide solid
    // angle reads a coarser mip, so sparse high-roughness taps pre-average instead of aliasing into
    // fireflies. Compute needs an explicit LOD (no implicit derivatives), and LOD 0 is the firefly case.
    uint envW, envH;
    envMap.GetDimensions(envW, envH);
    const float saTexel = 4.0 * PI / (6.0 * float(envW) * float(envH));

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = Hammersley(i, SAMPLE_COUNT);
        float3 H = ImportanceSampleGGX(Xi, N, pc.roughness);
        float3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0)
        {
            // V == R == N here, so HdotV and NdotH are the same dot product.
            float NdotH    = max(dot(N, H), 0.0);
            float pdf      = DistributionGGX(NdotH, pc.roughness) * NdotH / (4.0 * NdotH) + 0.0001;
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);
            float mip      = pc.roughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

            prefilteredColor += envMap.SampleLevel(L, max(mip, 0.0)).rgb * NdotL;
            totalWeight += NdotL;
        }
    }
    prefilteredColor /= max(totalWeight, 0.001);

    prefilteredOut[uint3(texCoord)] = float4(prefilteredColor, 1.0);
}
