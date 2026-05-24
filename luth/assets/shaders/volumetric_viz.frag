#version 450
#extension GL_GOOGLE_include_directive : enable

// Volumetric fog debug viz. Two modes selected by push constant:
//   0 = Density       — heat-map the density atlas (R channel × scale).
//   1 = In-Scatter    — raw RGB radiance of the integrated atlas × scale.
// Both sample SceneDepth to derive the per-fragment Wronski slice index, then read the 3D atlas
// at (screenUV, sliceW). Output is alpha-blended onto LDR so the scene shows through.

#include "common/globals.glsl"

layout(location = 0) in  vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

// Set 1: scene depth + the two atlases. b2 (volInScatter) parity-rewrites each frame to follow
// integrate's ping-pong write target — same primitive as composite's cycled b1.
layout(set = 1, binding = 0) uniform sampler2D u_SceneDepth;
layout(set = 1, binding = 1) uniform sampler3D u_VolDensity;
layout(set = 1, binding = 2) uniform sampler3D u_VolInScatter;

layout(push_constant) uniform PC {
    uint  mode;     // 0 = density, 1 = in-scatter
    float scale;    // user-tunable display multiplier
    float overlayAlpha;
} pc;

// Heat-map LUT — same shape as cluster_viz.frag for visual consistency across debug modes.
vec3 HeatColor(float t)
{
    t = clamp(t, 0.0, 1.0);
    if (t < 0.33)      return mix(vec3(0.0, 0.2, 0.7), vec3(0.0, 0.7, 0.2), smoothstep(0.0, 0.33, t));
    else if (t < 0.66) return mix(vec3(0.0, 0.7, 0.2), vec3(0.95, 0.85, 0.1), smoothstep(0.33, 0.66, t));
    else               return mix(vec3(0.95, 0.85, 0.1), vec3(0.95, 0.15, 0.05), smoothstep(0.66, 1.0, t));
}

void main()
{
    float ndcDepth = texture(u_SceneDepth, v_TexCoord).r;

    // Sky / far-plane pixels — skip; the atlas slice would clamp to slice=1.0 and tint the entire
    // sky with the fog readback.
    if (ndcDepth >= 0.9999) {
        outColor = vec4(0.0);
        return;
    }

    // Inverse of glm::perspectiveRH_ZO (Vulkan depth range 0..1).
    float viewZ  = (ubo.nearZ * ubo.farZ) / (ndcDepth * (ubo.nearZ - ubo.farZ) + ubo.farZ);
    float sliceW = clamp(log(max(viewZ, ubo.nearZ) / ubo.nearZ) / log(ubo.farZ / ubo.nearZ), 0.0, 1.0);
    vec3 uvw = vec3(v_TexCoord, sliceW);

    if (pc.mode == 0u) {
        float d = texture(u_VolDensity, uvw).r * pc.scale;
        outColor = vec4(HeatColor(clamp(d, 0.0, 1.0)), pc.overlayAlpha);
    } else {
        // In-scatter is HDR radiance — log10-encode for diagnostic usefulness. Raw values blow
        // out white near bright lights; log encoding spreads the dynamic range visibly.
        vec3 s = texture(u_VolInScatter, uvw).rgb * pc.scale;
        vec3 log_s = log(s + vec3(1.0)) * 0.434;  // log10(s+1)
        outColor = vec4(clamp(log_s, vec3(0.0), vec3(1.0)), pc.overlayAlpha);
    }
}
