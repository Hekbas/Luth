// Selection mask (non-skinned): renders selected entities from the camera. Un-jitters the projection so
// the mask + outline don't crawl under TAA. Per-draw model + jitter via push constant.
import globals;

struct VSIn { [[vk::location(0)]] float3 position; };

struct PC
{
    float4x4 model;
    uint     materialIndex;
    uint     shadeMode;
    uint     entityID;
    uint     boneOffset;
    float2   jitter;
};
[[vk::push_constant]] PC pc;

[shader("vertex")]
float4 main(VSIn vin) : SV_Position
{
    // Un-jitter the projection (negation of TaaJitter::ApplyJitter). GLSL proj[2][0]/[2][1] (col-major
    // element) -> Slang proj[0][2]/[1][2] (row-major index).
    float4x4 uProj = ubo.projection;
    uProj[0][2] -= pc.jitter.x * 2.0 / ubo.viewportSize.x;
    uProj[1][2] -= pc.jitter.y * 2.0 / ubo.viewportSize.y;
    return mul(uProj, mul(ubo.view, mul(pc.model, float4(vin.position, 1.0))));
}
