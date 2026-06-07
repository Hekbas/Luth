// SVGF (Schied 2017) shared helpers. Include AFTER common/globals.glsl — LinearizeDepth reads
// ubo.nearZ / ubo.farZ. SVGF shaders bind no light/reservoir state, so this is intentionally
// independent of restir_common.glsl (don't include both in one shader — OctDecode would collide).

#ifndef LUTH_SHADERS_COMMON_SVGF
#define LUTH_SHADERS_COMMON_SVGF

float SvgfLuminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Octahedral decode — inverse of slim_gbuffer.frag OctEncode (mirrors restir_common.glsl OctDecode).
vec3 SvgfOctDecode(vec2 enc) {
    enc = enc * 2.0 - 1.0;
    vec3 n = vec3(enc.xy, 1.0 - abs(enc.x) - abs(enc.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

// Reversed-Z-agnostic linear eye depth from the hardware depth value.
float SvgfLinearizeDepth(float d) {
    return (ubo.nearZ * ubo.farZ) / (ubo.farZ - d * (ubo.farZ - ubo.nearZ));
}

#endif
