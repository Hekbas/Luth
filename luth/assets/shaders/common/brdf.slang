// Cook-Torrance BRDF + GGX-VNDF importance sampling (Heitz 2018): single source of truth for the
// engine's surface BRDF, imported by the raster + RT shading paths. see arch/rendering-pipeline.md

static const float kPi = 3.14159265358979;   // internal; consumers get GI_PI from restir_gi_common

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

// Exact unpolarized dielectric Fresnel. cosI = cosine on the incident side (clamped >0); eta = n_i/n_t
// (relative IOR along the incident side: entering = 1/ior, exiting = ior). Returns 1 beyond the critical
// angle (total internal reflection) -- the branch that keeps interior bounces energy-correct.
public float F_Dielectric(float cosI, float eta)
{
    cosI = clamp(cosI, 0.0, 1.0);
    float s2 = eta * eta * (1.0 - cosI * cosI);   // sin^2(theta_t) via Snell
    if (s2 >= 1.0) return 1.0;                     // total internal reflection
    float cosT = sqrt(1.0 - s2);
    float rs = (eta * cosI - cosT) / (eta * cosI + cosT);
    float rp = (cosI - eta * cosT) / (cosI + eta * cosT);
    return 0.5 * (rs * rs + rp * rp);
}

// Anisotropic GGX NDF (Heitz). at/ab = roughness^2 along the tangent/bitangent; reduces to D_GGX at at==ab.
public float D_GGX_Aniso(float ToH, float BoH, float NoH, float at, float ab)
{
    float  a2 = at * ab;
    float3 d  = float3(ab * ToH, at * BoH, a2 * NoH);
    float  k  = a2 / max(dot(d, d), 1.0e-12);
    return a2 * k * k / kPi;               // (at*ab)^3 / (pi * dot(d,d)^2) == 1/(pi*at*ab*(..)^2)
}

// Height-correlated anisotropic Smith visibility (Heitz 2014); folds the 1/(4*NoV*NoL) denominator.
public float V_SmithGGXCorrelated_Aniso(float ToV, float BoV, float NoV,
                                        float ToL, float BoL, float NoL, float at, float ab)
{
    float lambdaV = NoL * length(float3(at * ToV, ab * BoV, NoV));
    float lambdaL = NoV * length(float3(at * ToL, ab * BoL, NoL));
    return 0.5 / max(lambdaV + lambdaL, 1.0e-6);
}

// Isotropic height-correlated Smith visibility (== the aniso form at at==ab==alpha); the clear-coat lobe.
public float V_SmithGGXCorrelated(float NoV, float NoL, float alpha)
{
    float a2 = alpha * alpha;
    float lambdaV = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float lambdaL = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(lambdaV + lambdaL, 1.0e-6);
}

// f(V,L)*NdotL*radiance: the shared evaluate-at-surface-point seam (radiance=1 yields f*NdotL).
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

// Extended surface: an anisotropic base lobe + an isotropic clear-coat lobe layered over it.
// invariant: at clearcoat==0 && anisotropy==0 this reduces to EvalBRDFTimesNdotL bit-for-bit (the fast
// path calls it verbatim), so materials that use neither are unchanged. see arch/rendering-pipeline.md
public struct SurfaceBRDF
{
    public float3 N, T, B;    // T/B pre-rotated by anisotropyRotation, orthonormalized vs N
    public float3 albedo;
    public float  metallic, roughness;
    public float  clearcoat, clearcoatRoughness;
    public float  anisotropy;   // 0 (or tangentless mesh) -> isotropic fast path
    public float  transmission; // dielectric transmission weight [0,1]; 0 = legacy fast paths
    public float  relEta;       // n_incident/n_transmitted on V's side (entering 1/ior, exiting ior)
}

// Build the shading frame once: Gram-Schmidt the tangent vs the (perturbed) N, restore the mesh handedness
// from the passed bitangent, rotate in the T/B plane. Zeroes anisotropy when no usable tangent exists.
public SurfaceBRDF MakeSurfaceBRDF(float3 N, float3 T, float3 B, bool hasTangent, float3 albedo,
                                   float metallic, float roughness, float clearcoat,
                                   float clearcoatRoughness, float anisotropy, float anisotropyRotation,
                                   float transmission, float ior, bool frontFace)
{
    SurfaceBRDF s;
    s.N = N; s.albedo = albedo; s.metallic = metallic; s.roughness = roughness;
    s.clearcoat = clearcoat; s.clearcoatRoughness = clearcoatRoughness;
    s.transmission = transmission;
    s.relEta = frontFace ? (1.0 / max(ior, 1.0)) : max(ior, 1.0);   // enter: 1/ior, exit: ior

    float3 To = T - dot(T, N) * N;                       // project the tangent onto the surface plane
    bool   ok = hasTangent && dot(To, To) > 1.0e-8;
    s.anisotropy = ok ? anisotropy : 0.0;                // no frame -> isotropic (aniso disabled)
    To = ok ? normalize(To) : float3(1.0, 0.0, 0.0);
    float3 Bo = cross(N, To);
    if (dot(Bo, B) < 0.0) Bo = -Bo;                      // preserve mesh handedness (raster tangent.w / RT tSign)

    float th = anisotropyRotation * (2.0 * kPi);
    float c = cos(th), sn = sin(th);
    s.T =  c * To + sn * Bo;
    s.B = -sn * To + c * Bo;
    return s;
}

// Layered evaluate-at-surface-point: anisotropic base lobe (when active) + isotropic clear-coat on top.
public float3 EvalSurfBRDFTimesNdotL(float3 L, float3 radiance, float3 V, SurfaceBRDF s)
{
    bool anisoActive = abs(s.anisotropy) > 1.0e-3;
    bool glassActive = s.transmission > 0.0;
    if (!anisoActive && s.clearcoat <= 0.0 && !glassActive)
        return EvalBRDFTimesNdotL(L, radiance, V, s.N, s.albedo, s.metallic, s.roughness);

    float NdotL = dot(s.N, L);
    if (NdotL <= 0.0) return float3(0.0);
    float  NoL = max(NdotL, 1.0e-4);

    float3 H   = normalize(V + L);
    float  NoV = max(dot(s.N, V), 1.0e-4);
    float  NoH = max(dot(s.N, H), 0.0);
    float  HoV = max(dot(H, V), 0.0);
    float3 F0  = lerp(float3(0.04), s.albedo, s.metallic);
    float3 F   = F_Schlick(HoV, F0);
    float  tw  = 0.0;
    if (glassActive)
    {
        // Interface reflection: blend the Schlick F toward the exact dielectric Fresnel by the transmission
        // weight, so a glass interface reflects with the correct IOR-driven Fresnel (incl. the TIR ceiling).
        tw = s.transmission * (1.0 - s.metallic);
        F  = lerp(F, float3(F_Dielectric(HoV, s.relEta)), tw);
    }

    float3 spec;
    if (anisoActive)
    {
        float a  = s.roughness * s.roughness;
        float at = clamp(a * (1.0 + s.anisotropy), 1.6e-3, 1.0);
        float ab = clamp(a * (1.0 - s.anisotropy), 1.6e-3, 1.0);
        float D  = D_GGX_Aniso(dot(s.T, H), dot(s.B, H), NoH, at, ab);
        float Vv = V_SmithGGXCorrelated_Aniso(dot(s.T, V), dot(s.B, V), NoV,
                                              dot(s.T, L), dot(s.B, L), NoL, at, ab);
        spec = D * Vv * F;                       // V folds 1/(4*NoV*NoL)
    }
    else
    {
        float D = D_GGX(s.N, H, s.roughness);    // base stays the legacy character; coat layers on top
        float G = G_Smith(s.N, V, L, s.roughness);
        spec = (D * G * F) / (4.0 * NoV * NoL);
    }

    float3 kD   = (float3(1.0) - F) * (1.0 - s.metallic);
    float3 diff = kD * s.albedo / kPi;
    if (glassActive) diff *= (1.0 - tw);   // diffuse energy re-routed into the transmission lobe (sampled, not evaluated here)
    float3 base = diff + spec;

    if (s.clearcoat > 0.0)
    {
        float ccR  = clamp(s.clearcoatRoughness, 0.04, 1.0);
        float Fc   = s.clearcoat * (0.04 + 0.96 * pow(1.0 - HoV, 5.0));    // F0=0.04 (IOR 1.5), weight-scaled
        float coat = D_GGX(s.N, H, ccR) * V_SmithGGXCorrelated(NoV, NoL, ccR * ccR) * Fc;
        base = base * (1.0 - Fc) + float3(coat);
    }

    return base * radiance * NdotL;
}

// Analytic Smith G1 for GGX alpha (unbiased VNDF pdf, distinct from the BRDF's approximate G).
public float G1_GGX(float NdotX, float alpha)
{
    float a2 = alpha * alpha;
    return 2.0 * NdotX / max(NdotX + sqrt(a2 + (1.0 - a2) * NdotX * NdotX), 1.0e-6);
}

// ---- Dielectric transmission (Walter 2007 rough-refraction GGX BTDF) ----
// Refraction half-vector, normalized into N's hemisphere. relEta = n_i/n_t on V's side; at L = -V
// (straight through) it collapses to N. A valid transmission config has VoH > 0, LoH < 0.
public float3 GlassHalfVector(float3 V, float3 L, float relEta, float3 N)
{
    float3 h = normalize(relEta * V + L);
    return (dot(h, N) < 0.0) ? -h : h;
}

// VNDF transmission pdf (reuses G1_GGX + D_GGX). d = relEta*VoH + LoH; transmission Jacobian |LoH|/d^2.
// Zero unless the sidedness is a valid refraction, so it composes into the PT mixture pdf cleanly.
public float PdfGlassBTDF(float3 N, float3 V, float3 L, float roughness, float relEta)
{
    float3 h   = GlassHalfVector(V, L, relEta, N);
    float  VoH = dot(V, h), LoH = dot(L, h);
    if (VoH <= 0.0 || LoH >= 0.0) return 0.0;
    float NoV = max(dot(N, V), 1.0e-4);
    float a   = roughness * roughness;
    float d   = relEta * VoH + LoH;
    float Dv  = G1_GGX(NoV, a) * VoH * D_GGX(N, h, roughness) / NoV;   // VNDF density
    return Dv * abs(LoH) / max(d * d, 1.0e-6);
}

// CONTRACT: symmetric BTDF -- the (eta_o/eta_i)^2 radiance-compression factor is intentionally omitted; a
// camera path through a closed interface enters AND exits so it cancels, and the raster refraction seam
// carries no such factor. PdfGlassBTDF + path_trace's mixture weight assume this convention. Separable
// Smith G2 = G1(V)*G1(L) keeps eval/pdf mutually consistent. see arch/rendering-pipeline.md
public float3 EvalGlassBTDFTimesNdotL(float3 L, float3 V, SurfaceBRDF s)
{
    float tw = s.transmission * (1.0 - s.metallic);
    if (tw <= 0.0) return float3(0.0);
    float3 h   = GlassHalfVector(V, L, s.relEta, s.N);
    float  VoH = dot(V, h), LoH = dot(L, h);
    if (VoH <= 0.0 || LoH >= 0.0) return float3(0.0);
    float NoV = max(dot(s.N, V), 1.0e-4);
    float NoL = abs(dot(s.N, L));
    float a   = s.roughness * s.roughness;
    float F   = F_Dielectric(VoH, s.relEta);
    float d   = s.relEta * VoH + LoH;
    float ft  = D_GGX(s.N, h, s.roughness) * G1_GGX(NoV, a) * G1_GGX(NoL, a)
              * VoH * (-LoH) / max(NoV * d * d, 1.0e-6);   // f_t * |NoL|, the |NoL| denom cancelled
    return s.albedo * (tw * (1.0 - F) * ft);               // baseColor = interface tint (glTF semantics)
}

// Sample a VNDF half-vector (Heitz 2018) given the tangent-space view Ve (N = +Z). Natively anisotropic:
// ax/ay = alpha along the tangent/bitangent (roughness^2 axes). The isotropic wrapper passes ax == ay.
public float3 SampleGGXVNDFAniso(float3 Ve, float ax, float ay, float u1, float u2)
{
    float3 Vh = normalize(float3(ax * Ve.x, ay * Ve.y, Ve.z));
    float  lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(Vh, T1);
    float  r = sqrt(u1); float phi = 2.0 * kPi * u2;
    float  t1 = r * cos(phi); float t2 = r * sin(phi);
    float  sl = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - sl) * sqrt(max(0.0, 1.0 - t1 * t1)) + sl * t2;
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    return normalize(float3(ax * Nh.x, ay * Nh.y, max(1.0e-6, Nh.z)));
}
public float3 SampleGGXVNDF(float3 Ve, float alpha, float u1, float u2)
{
    return SampleGGXVNDFAniso(Ve, alpha, alpha, u1, u2);
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

// Tangent-space anisotropic Smith G1 (w.z = cos). Reduces to G1_GGX at ax==ay.
public float G1_GGX_AnisoTS(float3 w, float ax, float ay)
{
    return 2.0 * w.z / max(w.z + length(float3(ax * w.x, ay * w.y, w.z)), 1.0e-6);
}

// Anisotropic VNDF pdf in tangent space (N = +Z): Vt/Lt are view/light in the rotated T/B/N frame.
public float PdfSpecAnisoTS(float3 Vt, float3 Lt, float ax, float ay)
{
    float3 Ht  = normalize(Vt + Lt);
    float  VoH = max(dot(Vt, Ht), 0.0);
    float  D   = D_GGX_Aniso(Ht.x, Ht.y, Ht.z, ax, ay);
    float  Dv  = G1_GGX_AnisoTS(Vt, ax, ay) * VoH * D / max(Vt.z, 1.0e-4);
    return Dv / (4.0 * max(VoH, 1.0e-4));
}
