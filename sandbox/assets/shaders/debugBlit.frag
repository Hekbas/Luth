#version 450

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_InputTexture;

// Simple Reinhard tone mapping for HDR -> LDR display
void main()
{
    vec3 hdr = texture(u_InputTexture, v_TexCoord).rgb;

    // Reinhard tone mapping
    vec3 ldr = hdr / (hdr + vec3(1.0));

    // Gamma correction
    ldr = pow(ldr, vec3(1.0 / 2.2));

    outColor = vec4(ldr, 1.0);
}
