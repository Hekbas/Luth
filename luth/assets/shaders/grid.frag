#version 450

// Infinite world-space grid (fractal LOD).
// Reuses fullscreen.vert; reconstructs the world ray per fragment, intersects
// the Y=0 plane, and draws three concentric grid frequencies whose alphas
// crossfade as the camera zooms — only ever showing two visible decades at once.

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform GridGlobals {
    mat4 viewProjection;
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
    float _pad;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D u_SceneDepth;

layout(push_constant) uniform GridPushConstants {
    vec4 axisXColor;     // RGB + intensity
    vec4 axisZColor;     // RGB + intensity
    vec4 gridColor;      // single neutral grid color (RGB + base alpha)
    float majorScale;    // base interval (1 m)
    float fadeStart;     // distance fade begin
    float fadeEnd;       // distance fade end
    float lineThickness; // line width scale
} pc;

// Anti-aliased grid line mask. Derivatives are taken on the unscaled world
// coord so every LOD draws lines with constant pixel-thickness.
float GridLine(vec2 worldPos, float scale, float thickness)
{
    vec2 coord = worldPos / scale;
    vec2 derivative = fwidth(worldPos) / scale;
    vec2 grid = abs(fract(coord - 0.5) - 0.5) / max(derivative, vec2(1e-8));
    float line = min(grid.x, grid.y);
    return 1.0 - min(line / max(thickness, 0.001), 1.0);
}

// Anti-aliased mask for the world X (z==0) and Z (x==0) axes.
vec2 AxisMask(vec2 p, float thickness)
{
    vec2 aa = fwidth(p) * thickness;
    float zAxis = 1.0 - smoothstep(0.0, aa.x, abs(p.x));
    float xAxis = 1.0 - smoothstep(0.0, aa.y, abs(p.y));
    return vec2(xAxis, zAxis);
}

void main()
{
    // Reconstruct world-space ray from NDC.
    vec2 ndc = v_TexCoord * 2.0 - 1.0;
    mat4 invVP = inverse(ubo.viewProjection);
    vec4 nearH = invVP * vec4(ndc, 0.0, 1.0);
    vec4 farH  = invVP * vec4(ndc, 1.0, 1.0);
    vec3 nearW = nearH.xyz / nearH.w;
    vec3 farW  = farH.xyz  / farH.w;
    vec3 rayDir = normalize(farW - nearW);

    // Intersect Y=0 plane.
    if (abs(rayDir.y) < 1e-6) discard;
    float t = -nearW.y / rayDir.y;
    if (t < 0.0) discard;
    vec3 hit = nearW + t * rayDir;

    // Distance fade gate.
    float distXZ = distance(hit.xz, ubo.cameraPos.xz);
    if (distXZ > pc.fadeEnd) discard;

    // Manual depth test against scene.
    vec4 gridClip = ubo.viewProjection * vec4(hit, 1.0);
    float gridNdcZ = gridClip.z / gridClip.w;
    float sceneDepth = texture(u_SceneDepth, v_TexCoord).r;
    if (sceneDepth < gridNdcZ) discard;

    // ----- Fractal three-LOD grid -----
    // LOD is driven by camera height (uniform across the screen → no seams).
    // We render three decades centered on the current LOD: fine, medium, coarse.
    // Their alphas crossfade with `lodFade` so that exactly two decades are
    // ever visible — and the levels match up perfectly across decade boundaries
    // so the transition is seamless and uses a single color (no hue pop).
    float baseSize     = max(pc.majorScale, 0.001);
    float subdivisions = 10.0;
    float camHeight    = max(abs(ubo.cameraPos.y), 1e-3);
    float rawLod       = log(camHeight / baseSize) / log(subdivisions);
    float lodLevel     = max(rawLod, 1.0); //clamp
    float lodFade      = fract(lodLevel);
    float lvl          = floor(lodLevel);

    float lodF = baseSize * pow(subdivisions, lvl - 1.0);
    float lodM = baseSize * pow(subdivisions, lvl);
    float lodC = baseSize * pow(subdivisions, lvl + 1.0);

    float lineF = GridLine(hit.xz, lodF, pc.lineThickness * 0.75);
    float lineM = GridLine(hit.xz, lodM, pc.lineThickness * 1.0);
    float lineC = GridLine(hit.xz, lodC, pc.lineThickness);

    // Crossfade alphas: F fades out as we zoom out, C fades in, M is constant.
    // Continuity: when lodFade jumps 1→0 across a decade boundary, the {M,C}
    // pair becomes the new {F,M}, and their alphas (1, lodFade≈0 → 1, 1) match
    // the previous (1-lodFade≈0, 1, 1) — no popping.
    float aF = lineF * pow(1.0 - lodFade, 2.0);
    float aM = lineM;
    float aC = lineC * lodFade;
    float lineAlpha = max(max(aF, aM), aC);

    vec4 color = vec4(pc.gridColor.rgb, lineAlpha * pc.gridColor.a);

    // Axis lines override grid color.
    vec2 axis = AxisMask(hit.xz, pc.lineThickness * 1.25);
    color.rgb = mix(color.rgb, pc.axisXColor.rgb, axis.x);
    color.a   = max(color.a, axis.x * pc.axisXColor.a);
    color.rgb = mix(color.rgb, pc.axisZColor.rgb, axis.y);
    color.a   = max(color.a, axis.y * pc.axisZColor.a);

    // Distance + grazing-angle fade.
    float globalFade = 1.0 - smoothstep(pc.fadeStart, pc.fadeEnd, distXZ);
    float angleFade = smoothstep(0.0, 0.5, abs(rayDir.y));
    color.a *= globalFade * angleFade;

    if (color.a <= 0.001) discard;
    outColor = color;
}
