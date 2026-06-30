// Infinite world-space grid (fractal LOD). Reconstructs the world ray per fragment, intersects Y=0, draws
// three crossfading grid decades. Reuses fullscreen vert. GridGlobals mirrors GlobalUniforms (Set 0 b0).
import globals;

[[vk::binding(1, 0)]] Sampler2D u_SceneDepth;

struct PC
{
    float4 axisXColor;
    float4 axisZColor;
    float4 gridColor;
    float  majorScale;
    float  fadeStart;
    float  fadeEnd;
    float  lineThickness;
    float2 jitter;
};
[[vk::push_constant]] PC pc;

struct FIn { [[vk::location(0)]] float2 uv; };

// Slang has no inverse() — cofactor/adjugate 4x4 (the un-jittered VP can't reuse ubo.invViewProjection).
float4x4 Inverse4x4(float4x4 m)
{
    float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2], a03 = m[0][3];
    float a10 = m[1][0], a11 = m[1][1], a12 = m[1][2], a13 = m[1][3];
    float a20 = m[2][0], a21 = m[2][1], a22 = m[2][2], a23 = m[2][3];
    float a30 = m[3][0], a31 = m[3][1], a32 = m[3][2], a33 = m[3][3];

    float b00 = a00*a11 - a01*a10, b01 = a00*a12 - a02*a10, b02 = a00*a13 - a03*a10;
    float b03 = a01*a12 - a02*a11, b04 = a01*a13 - a03*a11, b05 = a02*a13 - a03*a12;
    float b06 = a20*a31 - a21*a30, b07 = a20*a32 - a22*a30, b08 = a20*a33 - a23*a30;
    float b09 = a21*a32 - a22*a31, b10 = a21*a33 - a23*a31, b11 = a22*a33 - a23*a32;

    float det = b00*b11 - b01*b10 + b02*b09 + b03*b08 - b04*b07 + b05*b06;
    float invDet = 1.0 / det;

    float4x4 r;
    r[0][0] = ( a11*b11 - a12*b10 + a13*b09) * invDet;
    r[0][1] = (-a01*b11 + a02*b10 - a03*b09) * invDet;
    r[0][2] = ( a31*b05 - a32*b04 + a33*b03) * invDet;
    r[0][3] = (-a21*b05 + a22*b04 - a23*b03) * invDet;
    r[1][0] = (-a10*b11 + a12*b08 - a13*b07) * invDet;
    r[1][1] = ( a00*b11 - a02*b08 + a03*b07) * invDet;
    r[1][2] = (-a30*b05 + a32*b02 - a33*b01) * invDet;
    r[1][3] = ( a20*b05 - a22*b02 + a23*b01) * invDet;
    r[2][0] = ( a10*b10 - a11*b08 + a13*b06) * invDet;
    r[2][1] = (-a00*b10 + a01*b08 - a03*b06) * invDet;
    r[2][2] = ( a30*b04 - a31*b02 + a33*b00) * invDet;
    r[2][3] = (-a20*b04 + a21*b02 - a23*b00) * invDet;
    r[3][0] = (-a10*b09 + a11*b07 - a12*b06) * invDet;
    r[3][1] = ( a00*b09 - a01*b07 + a02*b06) * invDet;
    r[3][2] = (-a30*b03 + a31*b01 - a32*b00) * invDet;
    r[3][3] = ( a20*b03 - a21*b01 + a22*b00) * invDet;
    return r;
}

float GridLine(float2 worldPos, float scale, float thickness)
{
    float2 coord = worldPos / scale;
    float2 derivative = fwidth(worldPos) / scale;
    float2 grid = abs(frac(coord - 0.5) - 0.5) / max(derivative, float2(1e-8));
    float line = min(grid.x, grid.y);
    return 1.0 - min(line / max(thickness, 0.001), 1.0);
}

float2 AxisMask(float2 p, float thickness)
{
    float2 aa = fwidth(p) * thickness;
    float zAxis = 1.0 - smoothstep(0.0, aa.x, abs(p.x));
    float xAxis = 1.0 - smoothstep(0.0, aa.y, abs(p.y));
    return float2(xAxis, zAxis);
}

[shader("fragment")]
float4 main(FIn i) : SV_Target
{
    // Un-jitter the projection (GLSL proj[2][0]/[2][1] col-major → Slang proj[0][2]/[1][2]).
    float4x4 uProj = ubo.projection;
    uProj[0][2] -= pc.jitter.x * 2.0 / ubo.viewportSize.x;
    uProj[1][2] -= pc.jitter.y * 2.0 / ubo.viewportSize.y;
    float4x4 uVP = mul(uProj, ubo.view);

    float2 ndc = i.uv * 2.0 - 1.0;
    float4x4 invVP = Inverse4x4(uVP);
    float4 nearH = mul(invVP, float4(ndc, 0.0, 1.0));
    float4 farH  = mul(invVP, float4(ndc, 1.0, 1.0));
    float3 nearW = nearH.xyz / nearH.w;
    float3 farW  = farH.xyz  / farH.w;
    float3 rayDir = normalize(farW - nearW);

    if (abs(rayDir.y) < 1e-6) discard;
    float t = -nearW.y / rayDir.y;
    if (t < 0.0) discard;
    float3 hit = nearW + t * rayDir;

    float distXZ = distance(hit.xz, ubo.cameraPos.xz);
    if (distXZ > pc.fadeEnd) discard;

    float4 gridClip = mul(uVP, float4(hit, 1.0));
    float gridNdcZ = gridClip.z / gridClip.w;
    if (u_SceneDepth.Sample(i.uv).r < gridNdcZ) discard;

    float baseSize     = max(pc.majorScale, 0.001);
    float subdivisions = 10.0;
    float camHeight    = max(abs(ubo.cameraPos.y), 1e-3);
    float rawLod       = log(camHeight / baseSize) / log(subdivisions);
    float lodLevel     = max(rawLod, 1.0);
    float lodFade      = frac(lodLevel);
    float lvl          = floor(lodLevel);

    float lodF = baseSize * pow(subdivisions, lvl - 1.0);
    float lodM = baseSize * pow(subdivisions, lvl);
    float lodC = baseSize * pow(subdivisions, lvl + 1.0);

    float lineF = GridLine(hit.xz, lodF, pc.lineThickness * 0.75);
    float lineM = GridLine(hit.xz, lodM, pc.lineThickness * 1.0);
    float lineC = GridLine(hit.xz, lodC, pc.lineThickness);

    float aF = lineF * pow(1.0 - lodFade, 2.0);
    float aM = lineM;
    float aC = lineC * lodFade;
    float lineAlpha = max(max(aF, aM), aC);

    float4 color = float4(pc.gridColor.rgb, lineAlpha * pc.gridColor.a);

    float2 axis = AxisMask(hit.xz, pc.lineThickness * 1.25);
    color.rgb = lerp(color.rgb, pc.axisXColor.rgb, axis.x);
    color.a   = max(color.a, axis.x * pc.axisXColor.a);
    color.rgb = lerp(color.rgb, pc.axisZColor.rgb, axis.y);
    color.a   = max(color.a, axis.y * pc.axisZColor.a);

    float globalFade = 1.0 - smoothstep(pc.fadeStart, pc.fadeEnd, distXZ);
    float angleFade  = smoothstep(0.0, 0.5, abs(rayDir.y));
    color.a *= globalFade * angleFade;

    if (color.a <= 0.001) discard;
    return color;
}
