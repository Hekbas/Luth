#version 450

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_Input;

layout(push_constant) uniform PC {
    vec2 direction;  // (1/width, 0) for horizontal, (0, 1/height) for vertical
    float _pad[2];
} pc;

const float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main()
{
    vec3 result = texture(u_Input, v_TexCoord).rgb * weights[0];

    for (int i = 1; i < 5; ++i)
    {
        vec2 offset = pc.direction * float(i);
        result += texture(u_Input, v_TexCoord + offset).rgb * weights[i];
        result += texture(u_Input, v_TexCoord - offset).rgb * weights[i];
    }

    outColor = vec4(result, 1.0);
}
