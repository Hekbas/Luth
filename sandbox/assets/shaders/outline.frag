#version 450

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform usampler2D u_EntityID;

layout(push_constant) uniform OutlinePushConstants {
    uint  selectedEntityID;
    float outlineWidth;
    vec2  texelSize;     // 1.0 / resolution
    vec4  outlineColor;
};

void main()
{
    // No selection — fully transparent (blending discards)
    if (selectedEntityID == 0u)
        discard;

    uint center = texture(u_EntityID, v_TexCoord).r;
    bool centerIsSelected = (center == selectedEntityID);

    // Check neighbors for edge detection
    bool isEdge = false;
    float r = outlineWidth;

    for (float y = -r; y <= r; y += 1.0)
    {
        for (float x = -r; x <= r; x += 1.0)
        {
            if (x == 0.0 && y == 0.0) continue;

            vec2 offset = vec2(x, y) * texelSize;
            uint neighbor = texture(u_EntityID, v_TexCoord + offset).r;
            bool neighborIsSelected = (neighbor == selectedEntityID);

            if (centerIsSelected != neighborIsSelected)
            {
                isEdge = true;
                break;
            }
        }
        if (isEdge) break;
    }

    if (isEdge)
        outColor = outlineColor;
    else
        discard;
}
