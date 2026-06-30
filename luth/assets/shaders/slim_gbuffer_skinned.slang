// Slim G-buffer vertex (skinned). Reads the deformation seam: CURRENT clip from the CURR deformed region
// (+ TBN), PREVIOUS clip from the PREV region — the double-buffered deformed buffer replaces the dual-region
// bone skin for motion vectors. Output interface matches slim_gbuffer.frag. see arch/multi-queue.md
import globals;
import object_data;

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
VOut main(uint vid : SV_VertexID, uint baseInstance : SV_StartInstanceLocation)
{
    VOut o = (VOut)0;
    GPUObjectData obj = objects[baseInstance];

    // Guard: deformed buffer not ready (skinned BLAS still loading) — clip off-screen, no null deref.
    if (obj.deformedBufCurr == nullptr) { o.clipPos = float4(0.0, 0.0, 2.0, 1.0); return o; }

    uint   b        = vid * 13u;
    float3 dPos     = float3(obj.deformedBufCurr[b +  0u], obj.deformedBufCurr[b +  1u], obj.deformedBufCurr[b +  2u]);
    float3 dNrm     = float3(obj.deformedBufCurr[b +  3u], obj.deformedBufCurr[b +  4u], obj.deformedBufCurr[b +  5u]);
    float2 dUV0     = float2(obj.deformedBufCurr[b +  6u], obj.deformedBufCurr[b +  7u]);
    float2 dUV1     = float2(obj.deformedBufCurr[b +  8u], obj.deformedBufCurr[b +  9u]);
    float3 dTan     = float3(obj.deformedBufCurr[b + 10u], obj.deformedBufCurr[b + 11u], obj.deformedBufCurr[b + 12u]);
    float3 dPosPrev = float3(obj.deformedBufPrev[b +  0u], obj.deformedBufPrev[b +  1u], obj.deformedBufPrev[b +  2u]);

    o.uv0 = dUV0;
    o.uv1 = dUV1;

    // TBN against the current deformed normal/tangent (matches the RT hit basis + pbr_skinned).
    float3 N = normalize(mul(NormalMatrix(obj.model), dNrm));
    float3 T = normalize(mul((float3x3)obj.model, dTan));
    T = normalize(T - dot(T, N) * N);
    float3 Bt = cross(N, T);
    o.T = T; o.B = Bt; o.N = N;

    o.materialIndex = obj.materialIndex;

    o.currClip = mul(ubo.viewProjection,     mul(obj.model,     float4(dPos,     1.0)));
    o.prevClip = mul(ubo.prevViewProjection, mul(obj.prevModel, float4(dPosPrev, 1.0)));
    o.clipPos  = o.currClip;
    return o;
}
