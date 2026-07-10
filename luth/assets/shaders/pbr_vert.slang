// Forward-PBR vertex (non-skinned). Reads vertex attributes + per-object data, emits the VIn interface
// (pbr_shade.slang) consumed by pbr.slang + generated graph fragments.
import globals;
import object_data;

struct VSIn
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
    [[vk::location(3)]] float2 uv1;
    [[vk::location(4)]] float4 tangent;
    [[vk::location(5)]] float4 color;
};

struct VOut
{
    float4 clipPos : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
    [[vk::location(3)]] float3 T;
    [[vk::location(4)]] float3 B;
    [[vk::location(5)]] float3 N;
    [[vk::location(6)]] float2 uv1;
    [[vk::location(7)]] nointerpolation uint materialIndex;
    [[vk::location(8)]] nointerpolation uint shadeMode;
    [[vk::location(9)]] nointerpolation uint entityID;
    [[vk::location(10)]] float4 color;
};

[shader("vertex")]
VOut main(VSIn vin, uint baseInstance : SV_StartInstanceLocation)
{
    VOut o;
    GPUObjectData obj = objects[baseInstance];

    float4 worldPos = mul(obj.model, float4(vin.position, 1.0));
    o.worldPos = worldPos.xyz;
    o.uv0 = vin.uv0;
    o.uv1 = vin.uv1;
    o.color = vin.color;

    float3 N = normalize(mul(NormalMatrix(obj.model), vin.normal));
    float3 T = normalize(mul((float3x3)obj.model, vin.tangent.xyz));
    T = normalize(T - dot(T, N) * N);
    float3 Bt = cross(N, T) * vin.tangent.w;   // handedness sign fixes mirrored-UV bitangents
    o.normal = N; o.T = T; o.B = Bt; o.N = N;

    o.materialIndex = obj.materialIndex;
    o.shadeMode     = obj.shadeMode;
    o.entityID      = obj.entityID;

    o.clipPos = mul(ubo.viewProjection, worldPos);
    return o;
}
