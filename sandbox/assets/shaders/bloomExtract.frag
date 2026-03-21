#version 450

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_SceneColor;

layout(push_constant) uniform PC {
    float threshold;
    float _pad[3];
} pc;

void main()
{
    vec3 color = texture(u_SceneColor, v_TexCoord).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));

    // Soft knee extraction
    float soft = brightness - pc.threshold + 0.5;
    soft = clamp(soft, 0.0, 1.0);
    soft = soft * soft;

    float contribution = max(soft, step(pc.threshold, brightness));
    outColor = vec4(color * contribution, 1.0);
}
