// PPLL node layout + packing shared by pbr_oit_store.frag and oit_resolve.frag — single source
// of truth for the 16 B node (C++ sizes the pool as 16 + W*H*budget*16; header = {count, pad[3]}).

#ifndef LUTH_SHADERS_COMMON_OIT
#define LUTH_SHADERS_COMMON_OIT

#define OIT_EMPTY 0xFFFFFFFFu

// depthAlpha = (depth24 << 8) | alpha8 — one u32 sort key: ascending = nearest-first, alpha bits
// only break exact-depth ties. 24-bit fixed depth matches typical depth-buffer precision; keeping
// alpha here leaves entityID its full 32 bits (entt handles carry version bits — never truncate).
struct OITNode {
    uint  colorRGB9E5;  // shaded + fogged radiance (RGB9E5 shared-exponent HDR, no negatives)
    uint  depthAlpha;   // (fixed24(gl_FragCoord.z) << 8) | alpha8
    uint  entityID;     // v_EntityID — the resolve writes the nearest node's to the picking target
    uint  next;         // previous head index; OIT_EMPTY terminates
};

uint OitPackDepthAlpha(float depth01, float alpha) {
    uint d24 = uint(clamp(depth01, 0.0, 1.0) * 16777215.0);
    uint a8  = uint(clamp(alpha,   0.0, 1.0) * 255.0 + 0.5);
    return (d24 << 8) | a8;
}
float OitUnpackAlpha(uint depthAlpha) { return float(depthAlpha & 0xFFu) * (1.0 / 255.0); }

// RGB9E5 shared-exponent pack (Khronos layout: 9-bit mantissas, 5-bit exponent, bias 15).
uint OitPackRGB9E5(vec3 c) {
    const float kMax = 65408.0;   // (511/512) * 2^16 — format ceiling
    vec3  v    = clamp(c, vec3(0.0), vec3(kMax));
    float maxC = max(v.r, max(v.g, v.b));
    int   e    = (maxC < 1e-7) ? 0 : clamp(int(floor(log2(maxC))) + 16, 0, 31);
    float invScale = exp2(float(24 - e));
    uvec3 m    = min(uvec3(v * invScale + 0.5), uvec3(511u));
    return (uint(e) << 27) | (m.b << 18) | (m.g << 9) | m.r;
}
vec3 OitUnpackRGB9E5(uint p) {
    float scale = exp2(float(int(p >> 27) - 24));
    return vec3(float(p & 0x1FFu), float((p >> 9) & 0x1FFu), float((p >> 18) & 0x1FFu)) * scale;
}

#endif
