#version 450

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_DepthTexture;

layout(push_constant) uniform PushConstants {
    float nearPlane;
    float farPlane;
} pc;

// Linearize a depth value from [0,1] reverse-Z to [near, far] range
float LinearizeDepth(float d, float near, float far)
{
    // For reverse-Z: depth 0 = far, depth 1 = near
    return near * far / (far + d * (near - far));
}

void main()
{
    float depth = texture(u_DepthTexture, v_TexCoord).r;

    float near = pc.nearPlane;
    float far  = pc.farPlane;

    float linearDepth = LinearizeDepth(depth, near, far);

    // Normalize to [0, 1] for display
    float normalized = clamp(linearDepth / far, 0.0, 1.0);

    // Invert so close = bright, far = dark (more intuitive)
    normalized = 1.0 - normalized;

    outColor = vec4(vec3(normalized), 1.0);
}
