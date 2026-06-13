// Cook-Torrance BRDF + GGX-VNDF importance sampling (Heitz 2018) — single source of truth for the
// engine's surface BRDF, imported by the raster + RT shading paths. see arch/rendering-pipeline.md

static const float kPi = 3.14159265358979;   // internal — consumers get GI_PI from restir_gi_common

public float D_GGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness; float a2 = a * a;        // GGX alpha = roughness^2
    float NdotH = max(dot(N, H), 0.0); float NdotH2 = NdotH * NdotH;
    float d = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1.0e-7);
}

public float G_SchlickGGX(float NdotX, float roughness)
{
    float r = roughness + 1.0; float k = (r * r) / 8.0;       // direct-lighting k
    return NdotX / (NdotX * (1.0 - k) + k);
}

public float G_Smith(float3 N, float3 V, float3 L, float roughness)
{
    return G_SchlickGGX(max(dot(N, V), 0.0), roughness) * G_SchlickGGX(max(dot(N, L), 0.0), roughness);
}

public float3 F_Schlick(float cosT, float3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}

// f(V,L)·NdotL·radiance — the shared evaluate-at-surface-point seam (radiance=1 yields f·NdotL).
public float3 EvalBRDFTimesNdotL(float3 L, float3 radiance, float3 V, float3 N, float3 albedo, float metallic, float roughness)
{
    float3 H  = normalize(V + L);
    float3 F0 = lerp(float3(0.04), albedo, metallic);
    float  D  = D_GGX(N, H, roughness);
    float  G  = G_Smith(N, V, L, roughness);
    float3 F  = F_Schlick(max(dot(H, V), 0.0), F0);
    float3 spec = (D * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1.0e-4);
    float3 kD   = (float3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / kPi + spec) * radiance * max(dot(N, L), 0.0);
}

// Analytic Smith G1 for GGX alpha (unbiased VNDF pdf — distinct from the BRDF's approximate G).
public float G1_GGX(float NdotX, float alpha)
{
    float a2 = alpha * alpha;
    return 2.0 * NdotX / max(NdotX + sqrt(a2 + (1.0 - a2) * NdotX * NdotX), 1.0e-6);
}

// Sample a VNDF half-vector (Heitz 2018) given the tangent-space view Ve (N = +Z); alpha = roughness^2.
public float3 SampleGGXVNDF(float3 Ve, float alpha, float u1, float u2)
{
    float3 Vh = normalize(float3(alpha * Ve.x, alpha * Ve.y, Ve.z));
    float  lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(Vh, T1);
    float  r = sqrt(u1); float phi = 2.0 * kPi * u2;
    float  t1 = r * cos(phi); float t2 = r * sin(phi);
    float  sl = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - sl) * sqrt(max(0.0, 1.0 - t1 * t1)) + sl * t2;
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    return normalize(float3(alpha * Nh.x, alpha * Nh.y, max(1.0e-6, Nh.z)));
}

public float PdfDiffuse(float3 N, float3 L) { return max(dot(N, L), 0.0) / kPi; }

public float PdfSpec(float3 N, float3 V, float3 L, float roughness)
{
    float3 H = normalize(V + L);
    float  NdotV = max(dot(N, V), 0.0);
    float  VdotH = max(dot(V, H), 0.0);
    float  alpha = roughness * roughness;
    float  Dv = G1_GGX(NdotV, alpha) * VdotH * D_GGX(N, H, roughness) / max(NdotV, 1.0e-4);
    return Dv / (4.0 * max(VdotH, 1.0e-4));
}
