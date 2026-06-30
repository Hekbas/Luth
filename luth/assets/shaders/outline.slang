// Selection outline — exterior silhouette of the selection mask, depth-tested against the scene so
// occluded edges dim. Fullscreen.
[[vk::binding(0, 0)]] Sampler2D u_SelectionMask;
[[vk::binding(1, 0)]] Sampler2D u_SelectionDepth;
[[vk::binding(2, 0)]] Sampler2D u_SceneDepth;

struct PC
{
    float outlineWidth;
    float texelSizeX, texelSizeY;
    float outlineColorR, outlineColorG, outlineColorB, outlineColorA;
    float occludedAlpha;
};
[[vk::push_constant]] PC pc;

struct FIn { [[vk::location(0)]] float2 uv; };

[shader("fragment")]
float4 main(FIn i) : SV_Target
{
    float2 texelSize = float2(pc.texelSizeX, pc.texelSizeY);

    // Center inside the object → not an exterior edge.
    if (u_SelectionMask.Sample(i.uv).r > 0.5) discard;

    bool  isEdge = false;
    float r = pc.outlineWidth;
    float edgeSelDepth = 1.0;
    for (float y = -r; y <= r; y += 1.0)
    for (float x = -r; x <= r; x += 1.0)
    {
        if (x == 0.0 && y == 0.0) continue;
        float2 offset = float2(x, y) * texelSize;
        if (u_SelectionMask.Sample(i.uv + offset).r > 0.5)
        {
            isEdge = true;
            edgeSelDepth = min(edgeSelDepth, u_SelectionDepth.Sample(i.uv + offset).r);
        }
    }
    if (!isEdge) discard;

    float4 color = float4(pc.outlineColorR, pc.outlineColorG, pc.outlineColorB, pc.outlineColorA);
    if (u_SceneDepth.Sample(i.uv).r < edgeSelDepth)   // scene occludes the edge
        color.a *= pc.occludedAlpha;
    return color;
}
