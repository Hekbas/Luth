// Slim G-buffer vertex (non-skinned). Emits current + previous clip (motion vectors) + TBN. Output
// interface matches slim_gbuffer.frag.
import globals;
import object_data;

struct VSIn
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
    [[vk::location(3)]] float2 uv1;
    [[vk::location(4)]] float3 tangent;
};

struct VOut
{
    float4 clipPos : SV_Position;
    [[vk::location(0)]] float2 uv0;
    [[vk::location(1)]] float2 uv1;
    [[vk::location(2)]] float3 T;
    [[vk::location(3)]] float3 B;
    [[vk::location(4)]] float3 N;
    [[vk::location(5)]] float4 currClip;
    [[vk::location(6)]] float4 prevClip;
    [[vk::location(7)]] nointerpolation uint materialIndex;
};

[shader("vertex")]
VOut main(VSIn vin, uint baseInstance : SV_StartInstanceLocation)
{
    VOut o;
    GPUObjectData obj = objects[baseInstance];

    float4 modelPos = float4(vin.position, 1.0);
    float4 worldPos = mul(obj.model, modelPos);

    o.uv0 = vin.uv0;
    o.uv1 = vin.uv1;

    float3 N = normalize(mul(NormalMatrix(obj.model), vin.normal));
    float3 T = normalize(mul((float3x3)obj.model, vin.tangent));
    T = normalize(T - dot(T, N) * N);
    float3 Bt = cross(N, T);
    o.T = T; o.B = Bt; o.N = N;

    o.materialIndex = obj.materialIndex;

    o.currClip = mul(ubo.viewProjection,     worldPos);
    o.prevClip = mul(ubo.prevViewProjection, mul(obj.prevModel, modelPos));
    o.clipPos  = o.currClip;
    return o;
}
