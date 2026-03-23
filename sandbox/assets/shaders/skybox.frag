#version 450

layout(location = 0) in vec3 v_TexCoord;

layout(location = 0) out vec4 outColor;

// Pre-filtered env map at mip 0 = unfiltered environment
layout(set = 0, binding = 2) uniform samplerCube prefilteredMap;

void main()
{
    vec3 color = textureLod(prefilteredMap, v_TexCoord, 0.0).rgb;
    outColor = vec4(color, 1.0);
}
