// Slim G-buffer fragment: oct normal (RG16F) + roughness (R8) + NDC motion (RG16F) + material ID (R16U).
// Cutout alpha-tests the holes so RT shadow/reflection recon + GTAO sample the surface behind, not the card.
// Reuses the bounded material decode (material_bindings_raster); no local GPUMaterialData duplicate.
// see arch/rendering-pipeline.md
import globals;
import material;
import material_bindings_raster;   // GetMaterial + RasterFetch (Set 1 bindless, Set 2 material SSBO)

struct FIn
{
    [[vk::location(0)]] float2 uv0;
    [[vk::location(1)]] float2 uv1;
    [[vk::location(2)]] float3 T;
    [[vk::location(3)]] float3 B;
    [[vk::location(4)]] float3 N;
    [[vk::location(5)]] float4 currClip;
    [[vk::location(6)]] float4 prevClip;
    [[vk::location(7)]] nointerpolation uint materialIndex;
};

struct FOut
{
    [[vk::location(0)]] float2 normal     : SV_Target0;   // RG16F oct
    [[vk::location(1)]] float  roughness  : SV_Target1;   // R8
    [[vk::location(2)]] float2 motion     : SV_Target2;   // RG16F NDC delta
    [[vk::location(3)]] uint   materialID : SV_Target3;   // R16U
};

// Karis 2014 octahedral encode -> [0,1] for RG16F.
float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 enc = (n.z >= 0.0) ? n.xy : (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return enc * 0.5 + 0.5;
}

[shader("fragment")]
FOut main(FIn i, bool frontFacing : SV_IsFrontFace)
{
    FOut o;
    GPUMaterialData m = GetMaterial(i.materialIndex);
    RasterFetch rf;

    // Cutout alpha test (opaque carries alphaCutoff == 0 and never discards).
    float cutoutAlpha = m.color.a;
    if ((m.flags & FLAG_HAS_DIFFUSE) != 0u)
        cutoutAlpha *= rf.Sample(m.diffuseIndex, SelectUV(m.flags, UV_SHIFT_DIFFUSE, i.uv0, i.uv1)).a;
    if (cutoutAlpha < m.alphaCutoff) discard;

    // World shading normal: normal-mapped via TBN, else the interpolated vertex normal.
    float3 N;
    if ((m.flags & FLAG_HAS_NORMAL) != 0u)
    {
        float3 tn = DecodeTangentNormal(rf.Sample(m.normalIndex, SelectUV(m.flags, UV_SHIFT_NORMAL, i.uv0, i.uv1)).rgb);
        N = ApplyTangentNormal(tn, i.T, i.B, i.N);
    }
    else
    {
        N = normalize(i.N);
    }
    if (!frontFacing) N = -N;   // two-sided (cull-None cutout foliage)
    o.normal = OctEncode(N);

    // glTF roughness in the metalRough G channel.
    float roughness = m.roughness;
    if ((m.flags & FLAG_HAS_METALROUGH) != 0u)
        roughness = rf.Sample(m.metalRoughIndex, SelectUV(m.flags, UV_SHIFT_METALROUGH, i.uv0, i.uv1)).g;
    o.roughness = clamp(roughness, 0.04, 1.0);

    // Motion in NDC: source-side de-jittered so the attachment carries pure scene displacement.
    float2 jitterCurr = ubo.taaParams.zw  * 2.0 / ubo.viewportSize;
    float2 jitterPrev = ubo.prevJitter.xy * 2.0 / ubo.viewportSize;
    float2 currNDC = i.currClip.xy / i.currClip.w;
    float2 prevNDC = i.prevClip.xy / i.prevClip.w;
    o.motion = (currNDC + jitterCurr) - (prevNDC + jitterPrev);

    o.materialID = i.materialIndex & 0xFFFFu;
    return o;
}
