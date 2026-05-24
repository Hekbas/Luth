#version 450
#extension GL_GOOGLE_include_directive : enable

// Volumetric fog composite. Samples the front-to-back integrated in-scatter atlas (Wronski
// parameterization) at the fragment's view-space Z, applies analytic global distance + height
// fog, then emits a (fogColor, fogOpacity) pair shaped for the engine's standard alpha-blend
// equation (src*src.a + dst*(1-src.a)) — equivalent to Beer-Lambert:
//   final = sceneColor * T_total + scatter
// where fogOpacity = 1 - T_total and fogColor = scatter / fogOpacity (protected against /0).

#include "common/globals.glsl"

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D sceneDepth;
layout(set = 1, binding = 1) uniform sampler3D volInScatter;

// invView pushed each draw — avoids per-fragment inverse(ubo.view) (~40 ALU * full-screen).
layout(push_constant) uniform PC {
    mat4 invView;
} pc;

float DepthToViewZ(float ndcDepth) {
    // Inverse of glm::perspectiveRH_ZO (Vulkan depth range 0..1).
    return (ubo.nearZ * ubo.farZ) / (ndcDepth * (ubo.nearZ - ubo.farZ) + ubo.farZ);
}

// Slicing math replicated from volumetric_inject.comp.
float ViewZToAtlasSlice(float viewZ) {
    return clamp(log(max(viewZ, ubo.nearZ) / ubo.nearZ)
               / log(ubo.farZ / ubo.nearZ), 0.0, 1.0);
}

void main() {
    float ndcDepth = texture(sceneDepth, v_TexCoord).r;
    float viewZ    = DepthToViewZ(ndcDepth);
    bool  isSky    = ndcDepth >= 0.9999;

    // Volumetric sample.
    float sliceW = ViewZToAtlasSlice(viewZ);
    vec4 volS = texture(volInScatter, vec3(v_TexCoord, sliceW));
    vec3  volScatter  = volS.rgb;
    float volTransmit = volS.a;

    // World position from NDC + view-space Z (project diagonal unpack).
    vec2 ndcXY = v_TexCoord * 2.0 - 1.0;
    float vsZ = -viewZ;
    float vsX = ndcXY.x * (-vsZ) / ubo.projection[0][0];
    float vsY = ndcXY.y * (-vsZ) / ubo.projection[1][1];
    vec3 viewPos  = vec3(vsX, vsY, vsZ);
    vec3 worldPos = (pc.invView * vec4(viewPos, 1.0)).xyz;
    float camDist = length(worldPos - ubo.cameraPos);

    // Distance fog — analytic exponential.
    float dfEnabled = ubo.distanceFogParams.z;
    float dfDensity = ubo.distanceFogColorDensity.a;
    float dfStart   = ubo.distanceFogParams.x;
    float dfMaxOpa  = ubo.distanceFogParams.y;
    vec3  dfColor   = ubo.distanceFogColorDensity.rgb;
    float dfFactor  = (1.0 - exp(-dfDensity * max(camDist - dfStart, 0.0))) * dfEnabled;
    dfFactor = min(dfFactor, dfMaxOpa);
    float T_dist    = 1.0 - dfFactor;

    // Height fog — exponential drop-off below the reference height.
    float hfEnabled = ubo.heightFogParams.z;
    float hfDensity = ubo.heightFogColorDensity.a;
    float hfRefH    = ubo.heightFogParams.x;
    float hfFalloff = ubo.heightFogParams.y;
    vec3  hfColor   = ubo.heightFogColorDensity.rgb;
    float hDelta    = max(hfRefH - worldPos.y, 0.0);
    float hfFactor  = (1.0 - exp(-hfDensity * hDelta * hfFalloff)) * hfEnabled;
    float T_height  = 1.0 - hfFactor;

    float T_total = volTransmit * T_dist * T_height;
    vec3  scatter = volScatter
                  + dfColor * dfFactor * volTransmit
                  + hfColor * hfFactor * volTransmit * T_dist;

    // Re-shape for standard alpha blending: (fogColor, fogOpacity).
    float fogOpacity = clamp(1.0 - T_total, 0.0, 1.0);
    // Sky-pixel fog strength cap — scales fog opacity at the far slice so the skybox can stay
    // visible in dense fog. 1.0 = full fog on sky (skybox can disappear); 0.0 = no fog on sky.
    if (isSky) {
        float skyStrength = ubo.volTemporalParams.w;
        fogOpacity *= skyStrength;
    }
    vec3  fogColor   = (fogOpacity > 1e-5) ? scatter / fogOpacity : vec3(0.0);
    outColor = vec4(fogColor, fogOpacity);
}

