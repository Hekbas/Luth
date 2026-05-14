#version 450

// Per-vertex line endpoint. Position is world-space; color is RGBA8 (VK_FORMAT_R8G8B8A8_UNORM)
// auto-converted to normalized vec4. Push-constant viewProj transforms straight to clip space.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;

layout(push_constant) uniform PC {
    mat4 viewProj;
} u_PC;

layout(location = 0) out vec4 v_Color;

void main()
{
    gl_Position = u_PC.viewProj * vec4(a_Position, 1.0);
    v_Color     = a_Color;
}
