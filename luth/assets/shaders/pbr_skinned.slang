// Skinned forward-PBR vertex. Reads the deformation seam (post-skin object-space vertex) instead of
// skinning here; raster == RT by shared source. Emits the VIn interface (pbr_shade.slang) consumed by
// pbr.slang + the generated graph fragments.
import globals;
import object_data;

struct VOut
{
    float4 clipPos : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
    [[vk::location(3)]] float3 T;
    [[vk::location(4)]] float3 B;
    [[vk::location(5)]] float3 N;     // TBN 3rd column (== normal)
    [[vk::location(6)]] float2 uv1;
    [[vk::location(7)]] nointerpolation uint materialIndex;
    [[vk::location(8)]] nointerpolation uint shadeMode;
    [[vk::location(9)]] nointerpolation uint entityID;
    [[vk::location(10)]] float4 color;
};

[shader("vertex")]
VOut main(uint vid : SV_VertexID, uint baseInstance : SV_StartInstanceLocation)
{
    VOut o = (VOut)0;
    GPUObjectData obj = objects[baseInstance];

    // Guard: deformed buffer not ready (skinned BLAS still loading); clip off-screen, no null deref.
    if (obj.deformedBufCurr == nullptr) { o.clipPos = float4(0.0, 0.0, 2.0, 1.0); return o; }

    uint   b    = vid * 18u;
    float3 dPos = float3(obj.deformedBufCurr[b +  0u], obj.deformedBufCurr[b +  1u], obj.deformedBufCurr[b +  2u]);
    float3 dNrm = float3(obj.deformedBufCurr[b +  3u], obj.deformedBufCurr[b +  4u], obj.deformedBufCurr[b +  5u]);
    float2 dUV0 = float2(obj.deformedBufCurr[b +  6u], obj.deformedBufCurr[b +  7u]);
    float2 dUV1 = float2(obj.deformedBufCurr[b +  8u], obj.deformedBufCurr[b +  9u]);
    float4 dTan = float4(obj.deformedBufCurr[b + 10u], obj.deformedBufCurr[b + 11u], obj.deformedBufCurr[b + 12u], obj.deformedBufCurr[b + 13u]);
    float4 dCol = float4(obj.deformedBufCurr[b + 14u], obj.deformedBufCurr[b + 15u], obj.deformedBufCurr[b + 16u], obj.deformedBufCurr[b + 17u]);

    float4 worldPos = mul(obj.model, float4(dPos, 1.0));
    o.worldPos = worldPos.xyz;
    o.uv0      = dUV0;
    o.uv1      = dUV1;

    // Inverse-transpose normal matrix (non-uniform scale). TBN Gram-Schmidt re-orthogonalized.
    float3 N = normalize(mul(NormalMatrix(obj.model), dNrm));
    float3 T = normalize(mul((float3x3)obj.model, dTan.xyz));
    T = normalize(T - dot(T, N) * N);
    float3 Bt = cross(N, T) * dTan.w;   // handedness sign fixes mirrored-UV bitangents

    o.normal = N;
    o.T = T; o.B = Bt; o.N = N;
    o.color = dCol;

    o.materialIndex = obj.materialIndex;
    o.shadeMode     = obj.shadeMode;
    o.entityID      = obj.entityID;

    o.clipPos = mul(ubo.viewProjection, worldPos);
    return o;
}
