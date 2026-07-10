// Skinned camera-space depth prepass: reads the deformation seam (post-skin positions) so its depth
// matches the geometry + slim passes exactly (depth-EQUAL). Writes SceneDepth.
import globals;
import object_data;

[shader("vertex")]
float4 main(uint vid : SV_VertexID, uint baseInstance : SV_StartInstanceLocation) : SV_Position
{
    GPUObjectData obj = objects[baseInstance];

    // Guard: deformed buffer not ready (skinned BLAS still loading); clip off-screen, no null deref.
    if (obj.deformedBufCurr == nullptr) return float4(0.0, 0.0, 2.0, 1.0);

    uint   b    = vid * 18u;
    float3 dPos = float3(obj.deformedBufCurr[b + 0u], obj.deformedBufCurr[b + 1u], obj.deformedBufCurr[b + 2u]);

    return mul(ubo.viewProjection, mul(obj.model, float4(dPos, 1.0)));
}
