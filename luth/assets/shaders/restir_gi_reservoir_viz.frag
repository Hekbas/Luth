#version 460
#extension GL_GOOGLE_include_directive : enable

#include "common/restir_gi_common.glsl"

// Debug viz — heat-maps the ReSTIR GI spatial-reuse reservoir over LDR. M (confidence / merged sample
// count) drives the blue→green→yellow→red ramp; age (frames since the sample was refreshed) dims the
// result, so a converged-but-stale region reads dim-red and a fresh-low-confidence region bright-blue.
// Empty reservoirs (sky / no path sample) read near-black. Fullscreen triangle; flatIdx = y*W + x
// matches the GI compute passes' indexing (gl_FragCoord pixel == reservoir pixel, both full-res).

layout(location = 0) in  vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_SceneDepth;
layout(std430, set = 0, binding = 1) readonly buffer ReservoirBuffer { GIReservoir reservoirs[]; };

layout(push_constant) uniform PC {
    vec2  viewportSize;
    float mCap;     // M heat-ramp saturates here
    float ageCap;   // age fully dims here
} pc;

vec3 HeatColor(float t) {
    t = clamp(t, 0.0, 1.0);
    if (t < 0.33)      return mix(vec3(0.0, 0.2, 0.7), vec3(0.0, 0.7, 0.2), smoothstep(0.0, 0.33, t));
    else if (t < 0.66) return mix(vec3(0.0, 0.7, 0.2), vec3(0.95, 0.85, 0.1), smoothstep(0.33, 0.66, t));
    else               return mix(vec3(0.95, 0.85, 0.1), vec3(0.95, 0.15, 0.05), smoothstep(0.66, 1.0, t));
}

void main() {
    ivec2 px   = ivec2(gl_FragCoord.xy);
    ivec2 size = ivec2(pc.viewportSize);
    if (px.x >= size.x || px.y >= size.y) { outColor = vec4(0.0, 0.0, 0.0, 0.85); return; }

    float depth = texelFetch(u_SceneDepth, px, 0).r;
    if (depth >= 1.0) { outColor = vec4(0.0, 0.0, 0.0, 0.85); return; }   // sky → dark overlay

    uint flatIdx = uint(px.y) * uint(size.x) + uint(px.x);
    GIReservoir r = reservoirs[flatIdx];

    vec3 col;
    if (r.M == 0u) {
        col = vec3(0.04);                                  // empty reservoir → near-black
    } else {
        float mNorm   = (pc.mCap   > 0.0) ? float(r.M)   / pc.mCap   : 0.0;
        float ageNorm = (pc.ageCap > 0.0) ? float(r.age) / pc.ageCap : 0.0;
        float bright  = mix(1.0, 0.2, clamp(ageNorm, 0.0, 1.0));   // stale → dim
        col = HeatColor(mNorm) * bright;
    }
    outColor = vec4(col, 0.85);
}
