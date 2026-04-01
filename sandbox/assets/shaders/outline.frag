#version 450

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_SelectionMask;
layout(set = 0, binding = 1) uniform sampler2D u_SelectionDepth;
layout(set = 0, binding = 2) uniform sampler2D u_SceneDepth;

layout(push_constant) uniform OutlinePushConstants {
    float outlineWidth;
    float texelSizeX;
    float texelSizeY;
    float outlineColorR;
    float outlineColorG;
    float outlineColorB;
    float outlineColorA;
    float occludedAlpha;
};

void main()
{
    vec2 texelSize = vec2(texelSizeX, texelSizeY);

    float centerMask = texture(u_SelectionMask, v_TexCoord).r;
    bool centerIsSelected = (centerMask > 0.5);

    // 1. PUSH OUTSIDE: If the center pixel is part of the object, abort.
    // This ensures the outline only draws strictly on the exterior silhouette.
    if (centerIsSelected)
        discard;

    bool isEdge = false;
    float r = outlineWidth;
    
    // 2. DEPTH FIX: Track the depth of the selected neighbor.
    // Assuming standard Vulkan depth clear value is 1.0 (far)
    float edgeSelDepth = 1.0; 

    for (float y = -r; y <= r; y += 1.0)
    {
        for (float x = -r; x <= r; x += 1.0)
        {
            if (x == 0.0 && y == 0.0) continue;

            vec2 offset = vec2(x, y) * texelSize;
            float neighborMask = texture(u_SelectionMask, v_TexCoord + offset).r;
            bool neighborIsSelected = (neighborMask > 0.5);

            // Since we know the center is NOT selected, any selected neighbor makes this an edge.
            if (neighborIsSelected)
            {
                isEdge = true;
                
                // Sample the depth of the actual selected geometry at this neighbor's position
                float nDepth = texture(u_SelectionDepth, v_TexCoord + offset).r;
                
                // If multiple neighbors are selected, use the closest one to the camera
                edgeSelDepth = min(edgeSelDepth, nDepth); 
            }
        }
    }

    if (!isEdge)
        discard;

    vec4 color = vec4(outlineColorR, outlineColorG, outlineColorB, outlineColorA);

    // Sample the scene depth at this exact exterior pixel
    float sceneDepth = texture(u_SceneDepth, v_TexCoord).r;

    // 3. NO BIAS: Compare the scene background against the true edge depth.
    // If the scene geometry at this exterior pixel is closer to the camera 
    // than the edge of our selected object, the outline is occluded.
    if (sceneDepth < edgeSelDepth)
    {
        color.a *= occludedAlpha;
    }

    outColor = color;
}