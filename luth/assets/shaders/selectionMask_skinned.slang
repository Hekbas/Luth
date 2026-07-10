// Selection mask (skinned): VS-side Linear Blend Skinning from the 104 B skinned VB + Set 4 bones (this
// pass stays on isSkinned, not the deform seam). Un-jitters the projection like the non-skinned variant.
import globals;

struct VSIn
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
    [[vk::location(3)]] float2 uv1;
    [[vk::location(4)]] float4 tangent;
    [[vk::location(6)]] int4   boneIDs;      // color occupies loc 5, so bones shift to 6/7
    [[vk::location(7)]] float4 boneWeights;
};

[[vk::binding(0, 4)]] StructuredBuffer<float4x4> bones;   // Set 4: BoneMatrices

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
    float4x4 skinMatrix = (float4x4)0.0;
    for (int i = 0; i < 4; ++i)
        if (vin.boneIDs[i] >= 0)
            skinMatrix += vin.boneWeights[i] * bones[pc.boneOffset + uint(vin.boneIDs[i])];
    if (skinMatrix[0][0] == 0.0 && skinMatrix[1][1] == 0.0 && skinMatrix[2][2] == 0.0)
        skinMatrix = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);

    float4 skinnedPos = mul(skinMatrix, float4(vin.position, 1.0));

    float4x4 uProj = ubo.projection;
    uProj[0][2] -= pc.jitter.x * 2.0 / ubo.viewportSize.x;
    uProj[1][2] -= pc.jitter.y * 2.0 / ubo.viewportSize.y;
    return mul(uProj, mul(ubo.view, mul(pc.model, skinnedPos)));
}
