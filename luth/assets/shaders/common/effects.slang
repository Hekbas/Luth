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
    o.occlusion = lerp(a.occlusion, b.occlusion, t);
    o.emissive  = lerp(a.emissive,  b.emissive,  t);
    float3 n = lerp(a.normal, b.normal, t);
    o.normal = (dot(n, n) > 1e-8) ? normalize(n) : float3(0.0, 0.0, 1.0);
    return o;
}
