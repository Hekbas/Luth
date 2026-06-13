// Sun-shadow visibility — one 1-spp rayQuery per pixel toward the sun, writing an R8 mask the PBR pass
// samples when ShadowingMode == RtShadows. Cutout candidates (material_bindings_rt) carve alpha-tested holes.
// Occluded = the ray commits an alpha-passing hit before the sun; escapes = lit.
import globals;
import material;
import material_bindings_rt;

// Set 0 b6: TLAS (b0 = GlobalUniforms via globals). Set 1 b0: LightSSBO — only dirLight.direction read,
// byte-loaded to dodge std430 struct-layout drift. Set 2: scene depth + slim normal + R8 mask output.
[[vk::binding(6, 0)]] RaytracingAccelerationStructure topLevelAS;
[[vk::binding(0, 1)]] ByteAddressBuffer g_Lights;
[[vk::binding(0, 2)]] Sampler2D u_SceneDepth;
[[vk::binding(1, 2)]] Sampler2D u_SlimNormal;     // RG16F oct (Karis 2014)
[[vk::binding(2, 2)]] [[vk::image_format("r8")]] RWTexture2D<float> outShadowMask;

struct PC { GtGeomEntry* geomTable; };            // cutout alpha-test material fetch; 0 = none
[[vk::push_constant]] PC pc;

// Inverse of slim_gbuffer.frag's OctEncode. Decodes [0,1] RG16F → unit world-space normal.
float3 OctDecode(float2 enc)
{
    enc = enc * 2.0 - 1.0;
    float3 n = float3(enc.xy, 1.0 - abs(enc.x) - abs(enc.y));
    float t = max(-n.z, 0.0);
    n.x += (n.x >= 0.0) ? -t : t;
    n.y += (n.y >= 0.0) ? -t : t;
    return normalize(n);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int2  pixel = int2(tid.xy);
    uint2 size;  outShadowMask.GetDimensions(size.x, size.y);
    if (pixel.x >= int(size.x) || pixel.y >= int(size.y)) return;

    // CastShadows=false sentinel — bias[0] < 0 means shadows globally off. Write fully-lit and bail.
    if (ubo.shadowBias.x < 0.0) { outShadowMask[uint2(pixel)] = 1.0; return; }

    float2 uv    = (float2(pixel) + 0.5) / float2(size);
    float  depth = u_SceneDepth.SampleLevel(uv, 0.0).r;
    if (depth >= 1.0) { outShadowMask[uint2(pixel)] = 1.0; return; }   // skybox / cleared depth

    // ubo.invViewProjection = Inverse(viewProjection) (GlobalSubsystem) — depth→world; replaces the GLSL's
    // per-pixel inverse() Slang lacks. ubo.projection carries the Vulkan Y-flip, so plain uv*2-1 NDC works.
    float4 ndcPos   = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 worldH   = mul(ubo.invViewProjection, ndcPos);
    float3 worldPos = worldH.xyz / worldH.w;

    float3 N = OctDecode(u_SlimNormal.SampleLevel(uv, 0.0).rg);

    // LightSSBO .direction = light forward; negate to trace toward the sun.
    float3 L     = normalize(-asfloat(g_Lights.Load3(0)));
    float  NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) { outShadowMask[uint2(pixel)] = 0.0; return; }   // backface — self-shadowed

    // Wächter & Binder 2019 origin biasing (RT Gems Ch. 6): normal offset widens at grazing angles, light
    // pre-bias adds a constant margin along the ray so tMin can stay 0.
    float3 origin = worldPos
                  + N * (ubo.rtShadowParams.z * max(1.0 - NdotL, 0.1))
                  + L * ubo.rtShadowParams.y;

    // Drop the opaque flag so FORCE_NO_OPAQUE cutout instances surface as candidates; the loop confirms only
    // alpha-passing hits. ACCEPT_FIRST_HIT_AND_END_SEARCH → the first confirmed hit ends traversal.
    RayDesc ray; ray.Origin = origin; ray.TMin = 0.0; ray.Direction = L; ray.TMax = 10000.0;
    RayQuery<RAY_FLAG_NONE> rq;
    rq.TraceRayInline(topLevelAS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, GT_VIS_SOLID, ray);
    RtConfirmAlphaCandidates(rq, pc.geomTable);

    outShadowMask[uint2(pixel)] = (rq.CommittedStatus() == COMMITTED_NOTHING) ? 1.0 : 0.0;
}
