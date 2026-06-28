#version 450

// Selection mask pass — renders selected entities from camera viewpoint.
// Only position is needed; other attributes are present in the vertex buffer but ignored.
layout(location = 0) in vec3 a_Position;

layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 prevViewProjection;  // frame N-1's VP — motion vectors + TAA reprojection
    mat4 view;
    mat4 projection;
    vec3 cameraPos;
    float time;
    mat4 lightSpaceMatrix[4];
    vec4 cascadeSplitsViewZ;
    vec4 shadowBias;
    vec4 shadowNormalBias;
    vec4 cascadeTexelSize;
    float iblIntensity;
    float skyboxIntensity;
    float debugVisualizeCascades;
    float cascadeBlendWidth;
    vec2  viewportSize;       // pixels — cluster ID + screen-space recon
    float nearZ;
    float farZ;
} ubo;

layout(push_constant) uniform PushConstants {
    mat4 model;
    uint materialIndex;
    uint shadeMode;
    uint entityID;
    uint boneOffset;
    vec2 jitter;
} pc;

void main()
{
    // Un-jitter the projection so the mask (and its outline) don't crawl under TAA
    // (negation of TaaJitter::ApplyJitter).
    mat4 uProj = ubo.projection;
    uProj[2][0] -= pc.jitter.x * 2.0 / ubo.viewportSize.x;
    uProj[2][1] -= pc.jitter.y * 2.0 / ubo.viewportSize.y;
    gl_Position = (uProj * ubo.view) * pc.model * vec4(a_Position, 1.0);
}
