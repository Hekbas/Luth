// Karis14 / Pedersen16 TAA helpers — YCoCg conversion + clip_aabb + Blackman-Harris weights.
// Sources:
//   - Karis, "High Quality Temporal Supersampling" (SIGGRAPH 2014 Advances in Real-Time Rendering)
//   - Pedersen, "Temporal Reprojection Anti-Aliasing in INSIDE" (GDC 2016)
//   - playdead/temporal — MIT reference impl (Assets/Shaders/TemporalReprojection.shader)
//     https://github.com/playdeadgames/temporal

#ifndef LUTH_SHADERS_COMMON_TAA
#define LUTH_SHADERS_COMMON_TAA

// YCoCg ↔ RGB (BT.601 ratios). Chroma decorrelates from luma so the neighborhood-clamp step can
// narrow chroma extent separately — fixes the "purple fringe" ghost RGB AABB exhibits.
vec3 RGB_YCoCg(vec3 c)
{
    return vec3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
         0.5  * c.r              - 0.5  * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b
    );
}

vec3 YCoCg_RGB(vec3 c)
{
    float y = c.x, co = c.y, cg = c.z;
    return vec3(y + co - cg, y + cg, y - co - cg);
}

// playdead's clip_aabb — clip history toward AABB center along the (history - center) ray.
// Beats clamp because colors don't cluster in box corners (Karis SIGGRAPH 2014 slide).
// p = history (potentially outside AABB), q = AABB center (or current estimate).
vec4 clip_aabb(vec3 aabb_min, vec3 aabb_max, vec4 p, vec4 q)
{
    vec3 p_clip = 0.5 * (aabb_max + aabb_min);
    vec3 e_clip = 0.5 * (aabb_max - aabb_min) + vec3(1e-7);
    vec4 v_clip = p - vec4(p_clip, q.w);
    vec3 v_unit = v_clip.xyz / e_clip;
    vec3 a_unit = abs(v_unit);
    float ma_unit = max(max(a_unit.x, a_unit.y), a_unit.z);
    if (ma_unit > 1.0)
        return vec4(p_clip, q.w) + v_clip / ma_unit;
    return p;
}

// Blackman-Harris 3.3 Gaussian-fit weights (Karis "Reconstruction filter" slide). Center-heavy
// 3×3 reconstruction — preserves sub-pixel features TAA's natural blur would otherwise erase.
// Sum ≈ 1.0; applied to the CURRENT frame's neighborhood before the history blend.
const float k_BlackmanHarris3x3[9] = float[9](
    0.0094, 0.1145, 0.0094,
    0.1145, 0.5000, 0.1145,
    0.0094, 0.1145, 0.0094
);

#endif
