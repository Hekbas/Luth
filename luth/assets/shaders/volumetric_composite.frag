#version 450
#extension GL_GOOGLE_include_directive : enable

// Volumetric fog composite. All fog (FogVolume + distance + height) is voxelized in the inject
// pass — the atlas accumulates per-voxel density × tint × phase × shadow → integrate's volTransmit
// and volScatter contain the full result. Composite samples the atlas at the fragment's view-Z
// and emits a (fogColor, fogOpacity) pair shaped for the engine's standard alpha-blend equation
// (src*src.a + dst*(1-src.a)) — equivalent to Beer-Lambert:
//   final = sceneColor * volTransmit + scatter
// where fogOpacity = 1 - volTransmit and fogColor = volScatter / fogOpacity (protected against /0).

#include "common/globals.glsl"
#include "common/froxel.glsl"

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D sceneDepth;
layout(set = 1, binding = 1) uniform sampler3D volInScatter;
layout(set = 1, binding = 2) uniform sampler2D blueNoise;  // 64² R8 NEAREST+REPEAT

layout(push_constant) uniform PC {
    mat4 invView;          // reserved for future view-space sampling paths; unused today
} pc;

void main() {
    float ndcDepth = texture(sceneDepth, v_TexCoord).r;
    float viewZ    = FroxelDepthToViewZ(ndcDepth, ubo.nearZ, ubo.farZ);
    bool  isSky    = ndcDepth >= 0.9999;

    // Sample the integrated + resolved atlas. Blue-noise jitter on sliceW breaks up Wronski's
    // log-slice Z-banding by ±0.5 slices per fragment. Without TAA this shows as grain; with TAA
    // the dither integrates over ~6 frames into smooth gradients. Toggle via volScatterParams.y.
    float sliceW = FroxelViewZToSlice(viewZ, ubo.nearZ, ubo.farZ);
    if (ubo.volScatterParams.y != 0.0)
    {
        float dither = texture(blueNoise, gl_FragCoord.xy / 64.0).r;
        sliceW += (dither - 0.5) * (1.0 / float(textureSize(volInScatter, 0).z));
        sliceW  = clamp(sliceW, 0.0, 1.0);
    }
    vec4 volS = texture(volInScatter, vec3(v_TexCoord, sliceW));
    vec3  volScatter  = volS.rgb;
    float volTransmit = volS.a;

    // Distance-fog max-opacity still respected as an overall cap on how much fog can hide the
    // underlying scene (useful for keeping silhouettes visible in dense fog).
    float dfMaxOpa = ubo.distanceFogParams.y;

    float fogOpacity = clamp(1.0 - volTransmit, 0.0, max(dfMaxOpa, 0.001));
    // Sky-pixel fog strength cap — scales fog opacity at the far slice so the skybox can stay
    // visible in dense fog. 1.0 = full fog on sky (skybox can disappear); 0.0 = no fog on sky.
    if (isSky) {
        float skyStrength = ubo.volTemporalParams.w;
        fogOpacity *= skyStrength;
    }
    vec3 fogColor = (fogOpacity > 1e-5) ? volScatter / fogOpacity : vec3(0.0);
    outColor = vec4(fogColor, fogOpacity);
}

