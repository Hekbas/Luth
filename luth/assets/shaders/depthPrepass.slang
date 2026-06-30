// Camera-space depth-only prepass. Position only; writes SceneDepth for GTAO + the LESS_EQUAL forward test.
import globals;
import object_data;

struct VSIn { [[vk::location(0)]] float3 position; };

[shader("vertex")]
float4 main(VSIn vin, uint baseInstance : SV_StartInstanceLocation) : SV_Position
{
    GPUObjectData obj = objects[baseInstance];
    return mul(ubo.viewProjection, mul(obj.model, float4(vin.position, 1.0)));
}
