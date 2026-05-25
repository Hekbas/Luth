// Shared GlobalUniforms (Set 0 binding 0). Mirrors the C++ struct in
// luth/source/luth/scene/systems/RenderingSystem.h. ANY field reorder/add must update both sites.

#ifndef LUTH_SHADERS_COMMON_GLOBALS
#define LUTH_SHADERS_COMMON_GLOBALS

layout(set = 0, binding = 0) uniform GlobalUniforms {
    mat4 viewProjection;
    mat4 prevViewProjection;
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
    vec2  viewportSize;
    float nearZ;
    float farZ;
    vec4  distanceFogColorDensity;   // rgb = color, a = density
    vec4  distanceFogParams;         // x = start, y = maxOpacity, z = enabled, w = pad
    vec4  heightFogColorDensity;
    vec4  heightFogParams;           // x = refHeight, y = falloff, z = enabled, w = multiScatter
    vec4  volTemporalParams;         // x = anisotropy, y = temporalAlpha, z = sunFogAbsorptionSteps, w = skyFogStrength
    vec4  prevViewParams;            // x = prevNearZ, y = prevFarZ, z/w pad
    vec4  volNoiseParams;            // x = noiseScale (world frequency), y = noiseStrength (0..1), z/w pad
    vec4  volNoiseWind;              // xyz = wind direction × speed (m/s), w pad
    vec4  volScatterParams;          // x = scatteringIntensity (artistic post-canonical multiplier), yzw reserved
} ubo;

#endif
