// Skinned shadow depth: reads the deformation seam (post-skin positions) into the per-cascade light
// clip space. CPU pushes the cascade index per shadow-cascade invocation.
import globals;
import object_data;

struct PC { uint cascadeIndex; };
[[vk::push_constant]] PC pc;

[shader("vertex")]
float4 main(uint vid : SV_VertexID, uint baseInstance : SV_StartInstanceLocation) : SV_Position
{
    GPUObjectData obj = objects[baseInstance];

    // Guard: deformed buffer not ready (skinned BLAS still loading); clip off-screen, no null deref.
    if (obj.deformedBufCurr == nullptr) return float4(0.0, 0.0, 2.0, 1.0);

    uint   b    = vid * 18u;
    float3 dPos = float3(obj.deformedBufCurr[b + 0u], obj.deformedBufCurr[b + 1u], obj.deformedBufCurr[b + 2u]);

    return mul(ubo.lightSpaceMatrix[pc.cascadeIndex], mul(obj.model, float4(dPos, 1.0)));
}
