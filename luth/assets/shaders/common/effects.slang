// Surface-detail + layer-composition helpers for generated material-graph nodes (Triplanar, DetailNormal,
// LayerBlend). Imported CONDITIONALLY by generated modules (only when an effect/layer node is used) so
// pre-existing structures' canonical source stays hash-stable, matching graph_lib.slang.
// invariant: everything here is derivative-free and valid in fragment AND compute (raster == RT); the only
// tier difference is inside fetch.Sample (auto-mip raster vs SampleLevel 0 RT), the standing seam contract.
import material;

// World-space triplanar sample of one bindless map: three axis projections of the world position blended
// by the sharpened world-normal weights. Rides fetch.Sample, so raster auto-mips and RT samples LOD 0.
public float4 GraphTriplanar<F : ITexFetch>(F fetch, uint index, float3 wpos, float3 wnrm, float tiling)
{
    float3 w = abs(normalize(wnrm));
    w = w * w * w;                             // sharpen the seam between axes
    w /= max(w.x + w.y + w.z, 1e-4);
    float2 uvX = wpos.zy * tiling;
    float2 uvY = wpos.xz * tiling;
    float2 uvZ = wpos.xy * tiling;
    return fetch.Sample(index, uvX) * w.x
         + fetch.Sample(index, uvY) * w.y
         + fetch.Sample(index, uvZ) * w.z;
}

// Reoriented-Normal-Mapping blend of a detail tangent normal onto a base tangent normal (both signed,
// [-1,1]; the graph's Normal TextureSample already decoded them). strength scales the detail perturbation.
// see blending-in-detail (Barre-Brisebois & Hill).
public float4 GraphDetailNormal(float4 baseN, float4 detN, float strength)
{
    float3 b = baseN.xyz;
    float3 d = detN.xyz;
    d.xy *= strength;
    float3 t = b + float3(0.0, 0.0, 1.0);
    float3 u = d * float3(-1.0, -1.0, 1.0);
    float3 r = t * (dot(t, u) / max(t.z, 1e-4)) - u;
    return float4(normalize(r), 0.0);
}

// Per-channel mask blend of two whole surfaces (UE BlendMaterialAttributes, bounded to MaterialInputs).
// Normal is a normalized lerp - a spatial here-A-there-B mask blend, not a stack (DetailNormal stacks).
public MaterialInputs GraphLayerBlend(MaterialInputs a, MaterialInputs b, float t)
{
    MaterialInputs o;
    o.baseColor = lerp(a.baseColor, b.baseColor, t);
    o.metallic  = lerp(a.metallic,  b.metallic,  t);
    o.roughness = clamp(lerp(a.roughness, b.roughness, t), 0.04, 1.0);
    o.specular  = lerp(a.specular, b.specular, t);
    o.occlusion = lerp(a.occlusion, b.occlusion, t);
    o.emissive  = lerp(a.emissive,  b.emissive,  t);
    o.clearcoat          = lerp(a.clearcoat, b.clearcoat, t);
    o.clearcoatRoughness = clamp(lerp(a.clearcoatRoughness, b.clearcoatRoughness, t), 0.04, 1.0);
    o.anisotropy         = lerp(a.anisotropy, b.anisotropy, t);
    o.anisotropyRotation = lerp(a.anisotropyRotation, b.anisotropyRotation, t);
    o.sheenColor         = lerp(a.sheenColor, b.sheenColor, t);
    o.sheenRoughness     = clamp(lerp(a.sheenRoughness, b.sheenRoughness, t), 0.04, 1.0);
    o.subsurfaceColor     = lerp(a.subsurfaceColor, b.subsurfaceColor, t);
    o.subsurfaceRadius    = lerp(a.subsurfaceRadius, b.subsurfaceRadius, t);
    o.subsurfaceThickness = lerp(a.subsurfaceThickness, b.subsurfaceThickness, t);
    float3 n = lerp(a.normal, b.normal, t);
    o.normal = (dot(n, n) > 1e-8) ? normalize(n) : float3(0.0, 0.0, 1.0);
    return o;
}

// Parallax Occlusion Mapping: steep-march the tangent-space eye ray through the height field (white = up,
// surface depth = 1 - height), then secant-refine the bracket. p = (heightScale, minSteps, maxSteps, reserved).
// invariant: height taps are SampleLod 0 in both tiers (raster mips are undefined under the divergent march,
// RT has none) so displaced UVs match raster == RT; hard compile-time 64-step [loop] bound, guards -> baseUV.
public float4 GraphParallax<F : ITexFetch>(F fetch, uint heightIndex, float2 baseUV, float3 tvd, float4 p)
{
    const int MAX_MARCH_STEPS = 64;
    float absZ = abs(tvd.z);
    if (heightIndex == 0u || p.x <= 0.0 || absZ < 1e-4)
        return float4(baseUV, 0.0, 0.0);   // null/white slot, flat scale, or grazing-division blow-up

    // Fewer layers head-on, more at grazing; capped so the march spans the full [0,1] depth range inside
    // the hard bound. Heights saturate to [0,1], so a crossing bracket always exists before the cap.
    float numLayers  = clamp(lerp(p.z, p.y, saturate(absZ)), 1.0, float(MAX_MARCH_STEPS));
    float layerDepth = 1.0 / numLayers;
    // Luth's bitangent (cross(N,T)*sign, the normal-map basis) runs along -V in texture space, so the V
    // component is flipped to march along the +V gradient (else vertical parallax inverts). Both tiers build
    // B the same way, so the flip is identical (raster == RT).
    float2 deltaUV   = (float2(tvd.x, -tvd.y) / absZ) * (p.x * layerDepth);   // eye ray = -tvd; UV walks opposite

    float2 curUV    = baseUV;
    float  curDepth = 0.0;
    float  surf     = 1.0 - saturate(fetch.SampleLod(heightIndex, curUV, 0.0).r);
    float  prevSurf = surf;

    [loop]
    for (int i = 0; i < MAX_MARCH_STEPS; ++i)
    {
        if (curDepth >= surf) break;       // ray at or below the height field: bracket found
        prevSurf  = surf;
        curUV    -= deltaUV;
        curDepth += layerDepth;
        surf      = 1.0 - saturate(fetch.SampleLod(heightIndex, curUV, 0.0).r);
    }

    // Secant refinement: prev sample sat above the surface (before > 0), current at or below (after <= 0);
    // weight in [0,1] lands on the segment toward prevUV = curUV + deltaUV. Degenerate span keeps curUV.
    float after  = surf - curDepth;
    float before = prevSurf - (curDepth - layerDepth);
    float denom  = after - before;
    float weight = (abs(denom) > 1e-6) ? saturate(after / denom) : 0.0;
    return float4(curUV + deltaUV * weight, 0.0, 0.0);
}

// UV-space decal: inverse of the placement map uv = center + scale * R(rot) * (d - 0.5), with
// p = (centerU, centerV, scale, rotationRadians); decal.a lerps baseColor.rgb only (color-only v1).
// invariant: contribution is step-box-masked to d in [0,1]^2 - REPEAT samplers must never tile a decal.
public MaterialInputs GraphDecal<F : ITexFetch>(F fetch, uint decalIndex, MaterialInputs base, float2 uv, float4 p)
{
    if (decalIndex == 0u || abs(p.z) < 1e-4)
        return base;   // null/white slot or degenerate scale (1/scale blow-up)

    float2 q = (uv - p.xy) / p.z;                                    // undo translate + uniform scale
    float  c = cos(p.w), s = sin(p.w);
    float2 d = float2(c * q.x + s * q.y, c * q.y - s * q.x) + 0.5;   // R(-rot) inline, recentre at 0.5

    // Sampled unconditionally, masked after: keeps the raster tier's implicit gradients in uniform flow.
    float4 decal  = fetch.Sample(decalIndex, d);
    float2 inside = step(float2(0.0), d) * step(d, float2(1.0));
    float  mask   = decal.a * inside.x * inside.y;

    MaterialInputs o = base;
    o.baseColor.rgb = lerp(base.baseColor.rgb, decal.rgb, mask);
    return o;
}
