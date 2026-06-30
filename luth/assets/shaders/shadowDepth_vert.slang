// Shadow depth (non-skinned). Position into the per-cascade light clip space (CPU pushes the cascade).
import globals;
import object_data;

struct VSIn { [[vk::location(0)]] float3 position; };
struct PC { uint cascadeIndex; };
[[vk::push_constant]] PC pc;

[shader("vertex")]
float4 main(VSIn vin, uint baseInstance : SV_StartInstanceLocation) : SV_Position
{
    GPUObjectData obj = objects[baseInstance];
    return mul(ubo.lightSpaceMatrix[pc.cascadeIndex], mul(obj.model, float4(vin.position, 1.0)));
}
