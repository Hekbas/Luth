// Slang Phase-0 spike — SLANG PORT of slang_spike_gi.comp. Reimplements the geom_table.glsl hot path
// (buffer_reference vertex/index deref via raw BDA pointers + nonuniform bindless material/textures) plus
// the deterministic primary-ray shade, all in Slang. The whole point of the spike is that THIS file and
// the GLSL reference produce identical output: integer-derived channels bit-for-bit, FP shade within a
// few ULP under precise fp. Bindings are pinned with explicit [[vk::binding(b,set)]] to match the GLSL
// set numbers exactly (no ParameterBlock auto-assign — slang#8063). see spike #156

static const float kTMin      = 1.0e-3;
static const float kNormalEps = 1.0e-3;
static const float kPi        = 3.14159265358979;
static const uint  kMaxPoints = 8u;

static const uint GT_VIS_SOLID = 0x01u;
static const uint GT_VIS_ALL   = 0x03u;

static const uint GT_FLAG_HAS_NORMAL     = (1u << 0);
static const uint GT_FLAG_HAS_METALROUGH = (1u << 1);
static const uint GT_FLAG_HAS_OCCLUSION  = (1u << 2);
static const uint GT_FLAG_HAS_DIFFUSE    = (1u << 3);
static const uint GT_FLAG_HAS_EMISSIVE   = (1u << 4);
static const uint GT_UV_SHIFT_DIFFUSE    = 16u;
static const uint GT_UV_SHIFT_NORMAL     = 18u;
static const uint GT_UV_SHIFT_METALROUGH = 20u;
static const uint GT_UV_SHIFT_OCCLUSION  = 22u;

// Mirrors GtGeomEntry (geom_table.glsl) — 24 B. vbuf/ibuf are physical-storage-buffer pointers (the
// GL_EXT_buffer_reference2 equivalent); Slang lowers raw T* to a PhysicalStorageBuffer pointer.
struct GtGeomEntry
{
    float* vbuf;
    uint*  ibuf;
    uint   materialSlot;
    uint   vertexStride;
};

// Mirrors GPUMaterialData / GtMaterial — 80 B std430.
struct GtMaterial
{
    float4 color;
    uint   diffuseIndex, normalIndex, metalRoughIndex, occlusionIndex;
    uint   emissiveIndex, alphaIndex, specularIndex, thicknessIndex;
    float  metalness, roughness, alphaCutoff;
    uint   flags;
    float4 emissive;
};

struct HitSurface
{
    float3 ns;
    float3 ng;
    float3 baseColor;
    float3 emission;
    float  metalness;
    float  roughness;
    float  ao;
};

struct DirLight { float3 direction; float intensity; float3 color; };
struct PtLight  { float3 position;  float range;     float3 color; float intensity; };

[[vk::binding(6, 0)]] RaytracingAccelerationStructure        topLevelAS;
[[vk::binding(0, 1)]] ByteAddressBuffer                      g_Lights;
[[vk::binding(0, 2)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> u_OutImage;
[[vk::binding(0, 3)]] StructuredBuffer<GtMaterial>           gtMaterials;
[[vk::binding(0, 4)]] Sampler2D                              gtTextures[];

struct PushConsts
{
    float4x4     invViewProj;   // column-major (session default) — matches the GLSL std430 Mat4
    GtGeomEntry* geomTable;     // 64-bit BDA, same slot as the GLSL `GeomTable geomTable`
};
[[vk::push_constant]] PushConsts pc;

// LightBuffer header is { DirectionalLightData(32) , uint count(4) , uint pad[3](12) } then points[](32 each).
// Explicit byte loads avoid any Slang struct-layout drift vs the GLSL std430 SSBO.
DirLight LoadDirLight()
{
    DirLight d;
    d.direction = asfloat(g_Lights.Load3(0));
    d.intensity = asfloat(g_Lights.Load(12));
    d.color     = asfloat(g_Lights.Load3(16));
    return d;
}
uint LoadPointCount() { return g_Lights.Load(32); }
PtLight LoadPoint(uint j)
{
    uint b = 48u + j * 32u;
    PtLight p;
    p.position  = asfloat(g_Lights.Load3(b + 0));
    p.range     = asfloat(g_Lights.Load(b + 12));
    p.color     = asfloat(g_Lights.Load3(b + 16));
    p.intensity = asfloat(g_Lights.Load(b + 28));
    return p;
}

// Slang has no matrix inverse() (HLSL lineage). Cofactor/cross-product 3x3 inverse — rows of the result
// are (col1xcol2, col2xcol0, col0xcol1)/det. Feeds only the inverse-transpose normal matrix (FP channel),
// so tiny op-order differences vs GLSL's inverse() stay within the spike's ULP tolerance.
float3x3 Inverse3x3(float3x3 m)
{
    float3 a = float3(m[0][0], m[1][0], m[2][0]);   // column 0
    float3 b = float3(m[0][1], m[1][1], m[2][1]);   // column 1
    float3 c = float3(m[0][2], m[1][2], m[2][2]);   // column 2
    float3 r0 = cross(b, c);
    float3 r1 = cross(c, a);
    float3 r2 = cross(a, b);
    float invDet = 1.0 / dot(a, r0);
    return float3x3(r0 * invDet, r1 * invDet, r2 * invDet);
}

float3 PointLightRadiance(PtLight pl, float3 worldPos, out float3 L, out float dist)
{
    float3 toLight = pl.position - worldPos;
    dist = length(toLight);
    L    = (dist > 0.0) ? toLight / dist : float3(0.0, 1.0, 0.0);
    float atten   = 1.0 / max(dist * dist, 1e-4);
    float rolloff = pow(clamp(1.0 - dist / max(pl.range, 1e-4), 0.0, 1.0), 2.0);
    return pl.color * pl.intensity * atten * rolloff;
}

// Port of geom_table.glsl FetchHitSurface. GLSL `M * v` becomes Slang `mul(M, v)` (same logical product);
// GLSL `mat3(c0,c1,c2) * v` (column constructor) is expanded by hand to dodge HLSL's row-major ctor.
HitSurface FetchHitSurface(GtGeomEntry* geomTable, uint customIndex, uint primIndex, float2 bary, float3x4 o2w, float3 rayDir)
{
    HitSurface s;
    GtGeomEntry ge = geomTable[customIndex];
    uint sF = ge.vertexStride >> 2u;
    uint i0 = ge.ibuf[primIndex * 3u + 0u];
    uint i1 = ge.ibuf[primIndex * 3u + 1u];
    uint i2 = ge.ibuf[primIndex * 3u + 2u];

    float3 wgt = float3(1.0 - bary.x - bary.y, bary.x, bary.y);

    float3 p0 = float3(ge.vbuf[i0*sF + 0u], ge.vbuf[i0*sF + 1u], ge.vbuf[i0*sF + 2u]);
    float3 p1 = float3(ge.vbuf[i1*sF + 0u], ge.vbuf[i1*sF + 1u], ge.vbuf[i1*sF + 2u]);
    float3 p2 = float3(ge.vbuf[i2*sF + 0u], ge.vbuf[i2*sF + 1u], ge.vbuf[i2*sF + 2u]);
    float3 wp0 = mul(o2w, float4(p0, 1.0));
    float3 wp1 = mul(o2w, float4(p1, 1.0));
    float3 wp2 = mul(o2w, float4(p2, 1.0));
    s.ng = normalize(cross(wp1 - wp0, wp2 - wp0));
    if (dot(s.ng, rayDir) > 0.0) s.ng = -s.ng;

    float3 vn0 = float3(ge.vbuf[i0*sF + 3u], ge.vbuf[i0*sF + 4u], ge.vbuf[i0*sF + 5u]);
    float3 vn1 = float3(ge.vbuf[i1*sF + 3u], ge.vbuf[i1*sF + 4u], ge.vbuf[i1*sF + 5u]);
    float3 vn2 = float3(ge.vbuf[i2*sF + 3u], ge.vbuf[i2*sF + 4u], ge.vbuf[i2*sF + 5u]);
    float3 nsObj = wgt.x * vn0 + wgt.y * vn1 + wgt.z * vn2;
    float3x3 nrmM = transpose(Inverse3x3((float3x3)o2w));   // inverse-transpose normal matrix
    float3 nsW = mul(nrmM, nsObj);
    nsW = (dot(nsW, nsW) > 1.0e-12) ? normalize(nsW) : s.ng;

    float3 vt0 = float3(ge.vbuf[i0*sF + 10u], ge.vbuf[i0*sF + 11u], ge.vbuf[i0*sF + 12u]);
    float3 vt1 = float3(ge.vbuf[i1*sF + 10u], ge.vbuf[i1*sF + 11u], ge.vbuf[i1*sF + 12u]);
    float3 vt2 = float3(ge.vbuf[i2*sF + 10u], ge.vbuf[i2*sF + 11u], ge.vbuf[i2*sF + 12u]);
    float3 tanObj = wgt.x * vt0 + wgt.y * vt1 + wgt.z * vt2;

    float2 uv0 = wgt.x * float2(ge.vbuf[i0*sF + 6u], ge.vbuf[i0*sF + 7u])
               + wgt.y * float2(ge.vbuf[i1*sF + 6u], ge.vbuf[i1*sF + 7u])
               + wgt.z * float2(ge.vbuf[i2*sF + 6u], ge.vbuf[i2*sF + 7u]);
    float2 uv1 = wgt.x * float2(ge.vbuf[i0*sF + 8u], ge.vbuf[i0*sF + 9u])
               + wgt.y * float2(ge.vbuf[i1*sF + 8u], ge.vbuf[i1*sF + 9u])
               + wgt.z * float2(ge.vbuf[i2*sF + 8u], ge.vbuf[i2*sF + 9u]);

    GtMaterial m = gtMaterials[ge.materialSlot];
    s.baseColor = m.color.rgb;
    s.metalness = m.metalness;
    s.roughness = m.roughness;
    s.emission  = m.emissive.rgb * m.emissive.a;
    if ((m.flags & GT_FLAG_HAS_DIFFUSE) != 0u)
    {
        float2 uvd = (((m.flags >> GT_UV_SHIFT_DIFFUSE) & 3u) == 0u) ? uv0 : uv1;
        s.baseColor *= gtTextures[NonUniformResourceIndex(m.diffuseIndex)].SampleLevel(uvd, 0.0).rgb;
    }
    if ((m.flags & GT_FLAG_HAS_METALROUGH) != 0u)
    {
        float2 uvm = (((m.flags >> GT_UV_SHIFT_METALROUGH) & 3u) == 0u) ? uv0 : uv1;
        float3 mr  = gtTextures[NonUniformResourceIndex(m.metalRoughIndex)].SampleLevel(uvm, 0.0).rgb;
        s.roughness = mr.g;
        s.metalness = mr.b;
    }
    s.roughness = clamp(s.roughness, 0.04, 1.0);
    if ((m.flags & GT_FLAG_HAS_EMISSIVE) != 0u)
        s.emission *= gtTextures[NonUniformResourceIndex(m.emissiveIndex)].SampleLevel(uv0, 0.0).rgb;

    if ((m.flags & GT_FLAG_HAS_NORMAL) != 0u && dot(tanObj, tanObj) > 1.0e-12)
    {
        float3 T = normalize(mul((float3x3)o2w, tanObj));
        T = normalize(T - dot(T, nsW) * nsW);
        float3 B = cross(nsW, T);
        float2 uvn = (((m.flags >> GT_UV_SHIFT_NORMAL) & 3u) == 0u) ? uv0 : uv1;
        float3 tn  = gtTextures[NonUniformResourceIndex(m.normalIndex)].SampleLevel(uvn, 0.0).rgb * 2.0 - 1.0;
        nsW = normalize(T * tn.x + B * tn.y + nsW * tn.z);   // GLSL mat3(T,B,N)*tn, columns expanded
    }

    s.ao = 1.0;
    if ((m.flags & GT_FLAG_HAS_OCCLUSION) != 0u)
    {
        float2 uvo = (((m.flags >> GT_UV_SHIFT_OCCLUSION) & 3u) == 0u) ? uv0 : uv1;
        s.ao = gtTextures[NonUniformResourceIndex(m.occlusionIndex)].SampleLevel(uvo, 0.0).r;
    }
    if (dot(nsW, s.ng) < 0.0) nsW = -nsW;
    s.ns = nsW;
    return s;
}

bool AlphaTestCandidateHit(GtGeomEntry* geomTable, uint customIndex, uint primIndex, float2 bary)
{
    GtGeomEntry ge = geomTable[customIndex];
    GtMaterial  m  = gtMaterials[ge.materialSlot];
    if (m.alphaCutoff <= 0.0) return true;
    float alpha = m.color.a;
    if ((m.flags & GT_FLAG_HAS_DIFFUSE) != 0u)
    {
        uint sF  = ge.vertexStride >> 2u;
        uint off = (((m.flags >> GT_UV_SHIFT_DIFFUSE) & 3u) == 0u) ? 6u : 8u;
        uint i0 = ge.ibuf[primIndex*3u+0u], i1 = ge.ibuf[primIndex*3u+1u], i2 = ge.ibuf[primIndex*3u+2u];
        float3 wgt = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
        float2 uv = wgt.x * float2(ge.vbuf[i0*sF+off], ge.vbuf[i0*sF+off+1u])
                  + wgt.y * float2(ge.vbuf[i1*sF+off], ge.vbuf[i1*sF+off+1u])
                  + wgt.z * float2(ge.vbuf[i2*sF+off], ge.vbuf[i2*sF+off+1u]);
        alpha *= gtTextures[NonUniformResourceIndex(m.diffuseIndex)].SampleLevel(uv, 0.0).a;
    }
    return alpha >= m.alphaCutoff;
}

// Drive a ray query to completion, confirming only alpha-passing cutout candidates (opaque instances are
// hardware-auto-confirmed and never surface as candidates). Mirrors geom_table.glsl RtConfirmAlphaCandidates.
void RtConfirmAlphaCandidates(inout RayQuery<RAY_FLAG_NONE> q, GtGeomEntry* geomTable)
{
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE
            && AlphaTestCandidateHit(geomTable, q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
}

// 1 = blocked. SOLID mask so glass never blocks light (geom_table contract).
float Shadowed(float3 origin, float3 dir, float tMax)
{
    RayDesc ray; ray.Origin = origin; ray.TMin = kTMin; ray.Direction = dir; ray.TMax = tMax;
    RayQuery<RAY_FLAG_NONE> sq;
    sq.TraceRayInline(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, GT_VIS_SOLID, ray);
    RtConfirmAlphaCandidates(sq, pc.geomTable);
    return (sq.CommittedStatus() != COMMITTED_NOTHING) ? 1.0 : 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 size;
    u_OutImage.GetDimensions(size.x, size.y);
    int2 pixel = int2(tid.xy);
    if (pixel.x >= int(size.x) || pixel.y >= int(size.y)) return;

    float2 ndc    = ((float2(pixel) + 0.5) / float2(size)) * 2.0 - 1.0;
    float4 nearH  = mul(pc.invViewProj, float4(ndc, 0.0, 1.0));
    float4 farH   = mul(pc.invViewProj, float4(ndc, 1.0, 1.0));
    float3 origin = nearH.xyz / nearH.w;
    float3 dir    = normalize(farH.xyz / farH.w - origin);

    float3 outColor = float3(0.0, 0.0, 0.0);

    RayDesc ray; ray.Origin = origin; ray.TMin = kTMin; ray.Direction = dir; ray.TMax = 1e30;
    RayQuery<RAY_FLAG_NONE> rq;
    rq.TraceRayInline(topLevelAS, RAY_FLAG_NONE, GT_VIS_ALL, ray);
    RtConfirmAlphaCandidates(rq, pc.geomTable);
    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        uint     ci  = rq.CommittedInstanceID();
        uint     pi  = rq.CommittedPrimitiveIndex();
        float2   bc  = rq.CommittedTriangleBarycentrics();
        float3x4 o2w = rq.CommittedObjectToWorld3x4();
        float    t   = rq.CommittedRayT();
        float3   xs  = origin + dir * t;
        HitSurface hs = FetchHitSurface(pc.geomTable, ci, pi, bc, o2w, dir);

        float3 Lo = hs.emission;

        DirLight dl = LoadDirLight();
        if (dl.intensity > 0.0)
        {
            float3 Lsun = normalize(-dl.direction);
            float  ndl  = max(dot(hs.ns, Lsun), 0.0);
            if (ndl > 0.0)
            {
                float3 sOrig = xs + hs.ng * (kNormalEps * 0.5) + Lsun * kTMin;
                float  vis   = 1.0 - Shadowed(sOrig, Lsun, 1e30);
                Lo += (hs.baseColor / kPi) * dl.color * dl.intensity * ndl * vis;
            }
        }

        uint count = min(LoadPointCount(), kMaxPoints);
        for (uint j = 0u; j < count; ++j)
        {
            PtLight pl = LoadPoint(j);
            float3  Lw; float ld;
            float3  Li  = PointLightRadiance(pl, xs, Lw, ld);
            float   ndl = max(dot(hs.ns, Lw), 0.0);
            if (ndl > 0.0)
            {
                float3 sOrig = xs + hs.ng * (kNormalEps * 0.5) + Lw * kTMin;
                float  vis   = 1.0 - Shadowed(sOrig, Lw, max(ld - 2.0 * kTMin, kTMin));
                Lo += (hs.baseColor / kPi) * Li * ndl * vis;
            }
        }

        Lo += hs.baseColor * hs.ao * 0.03;
        outColor = Lo;
    }

    u_OutImage[uint2(pixel)] = float4(outColor, 1.0);
}
