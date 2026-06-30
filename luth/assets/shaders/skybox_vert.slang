// Skybox vertex. Cubemap direction = local position; view translation stripped so the box stays centred;
// z=w trick puts depth at 1.0 (renders behind everything).
import globals;

struct VSIn { [[vk::location(0)]] float3 position; };
struct VOut
{
    float4 clipPos : SV_Position;
    [[vk::location(0)]] float3 texCoord;
};

[shader("vertex")]
VOut main(VSIn vin)
{
    VOut o;
    o.texCoord = vin.position;
    float3 viewDir = mul((float3x3)ubo.view, vin.position);   // = mat4(mat3(view)) * pos, translation-free
    float4 pos = mul(ubo.projection, float4(viewDir, 1.0));
    o.clipPos = pos.xyww;
    return o;
}
