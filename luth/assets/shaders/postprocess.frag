#version 450

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D u_SceneColor;  // HDR
layout(set = 0, binding = 1) uniform sampler2D u_BloomColor;  // Blurred bloom

layout(set = 0, binding = 2) uniform PostProcessUBO {
    float bloomThreshold;
    float bloomStrength;
    float exposure;
    float contrast;

    float saturation;
    int   tonemapOp;
    float vignetteAmount;
    float vignetteHardness;

    float grainAmount;
    float sharpness;
    float chromaticAberration;
    float time;

    vec3 shadowBalance;
    vec3 midtoneBalance;
    vec3 highlightBalance;
} pp;

// --- Tonemapping operators ---

vec3 ACESFilm(vec3 x)
{
    float a = 2.51; float b = 0.03;
    float c = 2.43; float d = 0.59; float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 Reinhard(vec3 x)
{
    return x / (1.0 + x);
}

vec3 Uncharted2Helper(vec3 x)
{
    float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 Uncharted2(vec3 x)
{
    float W = 11.2;
    return Uncharted2Helper(x * 2.0) / Uncharted2Helper(vec3(W));
}

// CONTRACT: AgX returns LINEAR sRGB. The pow(color, 1/2.2) tail at end of main() does the display
// encode. The pow(color, vec3(2.2)) inside AgX is a *decode* — the sigmoid output sits in sRGB-OETF
// space and this re-linearizes it. DO NOT remove the tail gamma (washed-out image) and DO NOT flip
// the inner pow to 1/2.2 (too-dark image). Reference: Wrensch fit (iolite-engine.com), Three.js
// post-r161 (EaryChow review PR #27413), Blender 4.0+ AgX.
vec3 agxSigmoid(vec3 x)
{
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return + 15.5    * x4 * x2
           - 40.14   * x4 * x
           + 31.96   * x4
           - 6.868   * x2 * x
           + 0.4298  * x2
           + 0.1191  * x
           - 0.00232;
}

vec3 agxLookPunchy(vec3 val)
{
    // Blender's "Punchy" preset (slope 1.0, power 1.35, sat 1.4) — ASC CDL applied between
    // sigmoid and outset matrix. Lifts contrast and saturation off the neutral AgX base.
    const vec3 lw    = vec3(0.2126, 0.7152, 0.0722);
    float      luma  = dot(val, lw);
    vec3       slope = vec3(1.0);
    vec3       power = vec3(1.35);
    float      sat   = 1.4;
    val = pow(val * slope, power);
    return luma + sat * (val - luma);
}

vec3 AgXBase(vec3 color, bool punchy)
{
    const mat3 AgXInsetMatrix = mat3(
        vec3(0.856627153315983,  0.0951212405381588, 0.0482516061458583),
        vec3(0.137318972929847,  0.761241990602591,  0.101439036467562),
        vec3(0.11189821299995,   0.0767994186031903, 0.811302368396859)
    );
    const mat3 AgXOutsetMatrix = mat3(
        vec3( 1.1271005818144368,   -0.1413297634984383,  -0.14132976349843826),
        vec3(-0.11060664309660323,   1.157823702216272,   -0.11060664309660294),
        vec3(-0.016493938717834573, -0.016493938717834257, 1.2519364065950405)
    );
    const float AgxMinEv = -12.47393;
    const float AgxMaxEv =   4.026069;

    color = AgXInsetMatrix * color;
    color = max(color, vec3(1e-10));
    color = log2(color);
    color = (color - AgxMinEv) / (AgxMaxEv - AgxMinEv);
    color = clamp(color, 0.0, 1.0);
    color = agxSigmoid(color);
    if (punchy) color = agxLookPunchy(color);
    color = AgXOutsetMatrix * color;
    color = pow(max(color, vec3(0.0)), vec3(2.2));  // sRGB EOTF decode — see CONTRACT above
    return color;
}

vec3 AgX(vec3 x)       { return AgXBase(x, false); }
vec3 AgXPunchy(vec3 x) { return AgXBase(x, true); }

void main()
{
    vec2 uv = v_TexCoord;

    // --- Chromatic Aberration ---
    vec3 color;
    if (pp.chromaticAberration > 0.0)
    {
        vec2 dir = uv - vec2(0.5);
        float dist = length(dir);
        vec2 offset = dir * dist * pp.chromaticAberration;
        color.r = texture(u_SceneColor, uv + offset).r;
        color.g = texture(u_SceneColor, uv).g;
        color.b = texture(u_SceneColor, uv - offset).b;
    }
    else
    {
        color = texture(u_SceneColor, uv).rgb;
    }

    // --- Bloom composite ---
    vec3 bloom = texture(u_BloomColor, uv).rgb;
    color += bloom * pp.bloomStrength;

    // --- Exposure ---
    color *= pp.exposure;

    // --- Tone mapping ---
    if (pp.tonemapOp == 1)      color = Reinhard(color);
    else if (pp.tonemapOp == 2) color = ACESFilm(color);
    else if (pp.tonemapOp == 3) color = Uncharted2(color);
    else if (pp.tonemapOp == 4) color = AgX(color);
    else if (pp.tonemapOp == 5) color = AgXPunchy(color);
    // else linear (no mapping)

    // --- Contrast ---
    color = mix(vec3(0.18), color, pp.contrast);

    // --- Saturation ---
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = mix(vec3(luma), color, pp.saturation);

    // --- Vignette ---
    if (pp.vignetteAmount > 0.0)
    {
        float dist = length(uv - vec2(0.5)) * 1.414;
        float vig = smoothstep(1.0 - pp.vignetteHardness, 1.0, dist);
        color *= 1.0 - vig * pp.vignetteAmount;
    }

    // --- Film grain ---
    if (pp.grainAmount > 0.0)
    {
        float noise = fract(sin(dot(uv * pp.time, vec2(12.9898, 78.233))) * 43758.5453);
        color += (noise - 0.5) * pp.grainAmount;
    }

    // --- Gamma correction (linear -> sRGB) ---
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
