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

// ---- Estevez-Kulla 2017 production sheen (fabric / velvet / banners) ----
// "Charlie" sheen NDF: (2 + 1/a) * sin^(1/a) / (2*pi), a = sheenRoughness^2 (glTF KHR_materials_sheen /
// Filament perceptual remap). invariant: int D*NoH dwh == 1 for any alpha; the sin2h floor 2^-7 keeps
// the pow finite. Caller clamps sheenRoughness (coat-style [0.04,1]).
public float D_Charlie(float sheenRoughness, float NoH)
{
    float a = sheenRoughness * sheenRoughness;
    float invAlpha = 1.0 / a;
    float sin2h = max(1.0 - NoH * NoH, 0.0078125);
    return (2.0 + invAlpha) * pow(sin2h, invAlpha * 0.5) / (2.0 * kPi);
}

// Soft shadowing-masking: Smith Lambda numerically fit in log space (mirrored about x=0.5), coefficient
// sets at alpha 0/1 interpolated by (1-alpha)^2. Coeffs match the KHR_materials_sheen reference impl.
float LambdaSheenL(float x, float aG)
{
    float t = (1.0 - aG) * (1.0 - aG);
    float a = lerp(21.5473, 25.3245, t);
    float b = lerp(3.82987, 3.32435, t);
    float c = lerp(0.19823, 0.16801, t);
    float d = lerp(-1.97760, -1.27393, t);
    float e = lerp(-4.32054, -4.85967, t);
    return a / (1.0 + b * pow(x, c)) + d * x + e;
}
float LambdaSheen(float cosT, float aG)
{
    return (cosT < 0.5) ? exp(LambdaSheenL(cosT, aG))
                        : exp(2.0 * LambdaSheenL(0.5, aG) - LambdaSheenL(1.0 - cosT, aG));
}

// Height-UNcorrelated Smith combine (the paper's form; do NOT swap to the height-correlated one -- the fit
// was derived for this). Folds 1/(4*NoV*NoL) like the other V_*; the [0,1] clamp bounds grazing energy AND
// the PT f/pdf ratio. Cheaper fallback (unused): V_Neubelt = 1/(4*(NoL+NoV-NoL*NoV)), whose albedo
// overshoots 1 at grazing where the EK fit stays <= ~0.9.
public float V_Charlie(float sheenRoughness, float NoV, float NoL)
{
    float aG = sheenRoughness * sheenRoughness;
    float G  = 1.0 / (1.0 + LambdaSheen(NoV, aG) + LambdaSheen(NoL, aG));
    return clamp(G / (4.0 * NoV * NoL), 0.0, 1.0);
}

// Directional albedo of the shipped sheen lobe (D_Charlie * V_Charlie incl. its clamp): analytic fit
// (mean err ~2%, |abs| <= 0.03 for r in [0.15,1]). Bespoke because published fits target the Neubelt lobe
// (differs up to ~1.7x). One fit feeds the base energy comp + IBL sheen + the PT lobe weight, so all three
// paths stay matched. Self-clamps r (the 1/r^2 basis diverges outside the fit domain).
public float E_Sheen(float NoV, float sheenRoughness)
{
    float r  = clamp(sheenRoughness, 0.04, 1.0);
    float ir = 1.0 / r;
    float q0 = ((0.206010 * r + 0.226792) * r - 0.795040) + (0.168545 - 0.004546 * ir) * ir;
    float q1 = ((-5.311925 * r + 8.385109) * r - 4.158626) + (-0.651188 + 0.007145 * ir) * ir;
    float q2 = ((1.632468 * r - 0.764938) * r - 1.466601) + (0.951670 - 0.212060 * ir) * ir;
    float cap = ((-3.086316 * r + 6.473346) * r - 4.160183) * r + 1.524600;
    return min(min(exp(q0 + (q1 + q2 * NoV) * NoV), cap), 1.0);
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

// Extended surface: an anisotropic base lobe + a clear-coat lobe + a dielectric-transmission lobe + a
// production-sheen lobe layered over the base. invariant: with every extension weight zero (clearcoat==0
// && anisotropy==0 && transmission==0 && sheenColor==0) this reduces to EvalBRDFTimesNdotL bit-for-bit
// (the fast path calls it verbatim), so materials that use none are unchanged. see arch/rendering-pipeline.md
public struct SurfaceBRDF
{
    public float3 N, T, B;    // T/B pre-rotated by anisotropyRotation, orthonormalized vs N
    public float3 albedo;
    public float  metallic, roughness;
    public float  clearcoat, clearcoatRoughness;
    public float  anisotropy;   // 0 (or tangentless mesh) -> isotropic fast path
    public float  transmission; // dielectric transmission weight [0,1]; 0 = legacy fast paths
    public float  relEta;       // n_incident/n_transmitted on V's side (entering 1/ior, exiting ior)
    public float3 sheenColor;   // production-sheen reflectance (linear); 0 = legacy fast paths
    public float  sheenRoughness;
    public float3 subsurfaceColor;  // SSS diffusion surface albedo A (linear, per-RGB); 0 = legacy fast paths
    public float  scatterRadius;    // SSS mean-free-path (world units)
    public float  thickness;        // SSS translucency path-length proxy (glTF thicknessFactor); thin -> more glow
}

// Build the shading frame once: Gram-Schmidt the tangent vs the (perturbed) N, restore the mesh handedness
// from the passed bitangent, rotate in the T/B plane. Zeroes anisotropy when no usable tangent exists.
public SurfaceBRDF MakeSurfaceBRDF(float3 N, float3 T, float3 B, bool hasTangent, float3 albedo,
                                   float metallic, float roughness, float clearcoat,
                                   float clearcoatRoughness, float anisotropy, float anisotropyRotation,
                                   float transmission, float ior, bool frontFace,
                                   float3 sheenColor, float sheenRoughness,
                                   float3 subsurfaceColor, float scatterRadius, float thickness)
{
    SurfaceBRDF s;
    s.N = N; s.albedo = albedo; s.metallic = metallic; s.roughness = roughness;
    s.clearcoat = clearcoat; s.clearcoatRoughness = clearcoatRoughness;
    s.transmission = transmission;
    s.relEta = frontFace ? (1.0 / max(ior, 1.0)) : max(ior, 1.0);   // enter: 1/ior, exit: ior
    s.sheenColor = sheenColor; s.sheenRoughness = sheenRoughness;
    s.subsurfaceColor = subsurfaceColor; s.scatterRadius = scatterRadius; s.thickness = thickness;

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

// ---- Subsurface scattering (skin / wax / marble): local diffusion approx + the PT random-walk inversion ----
// Burley Eq.5 (searchlight) shape s(A) = 1.85 - A + 7|A-0.8|^3 -- monotone-decreasing, so the per-channel
// profile width A/s(A) is widest for the brightest channel (red bleeds furthest) under one scalar radius.
static const float kSssWrap    = 0.5;   // scatterRadius (world units) -> [0,1] terminator-wrap scale (tuning)
static const float kSssDistort = 0.3;   // back-scatter light-bend toward N (Frostbite distortion, tuning)
static const float kSssPower   = 4.0;   // back-scatter view falloff exponent (tuning)

float3 SssProfileShape(float3 A) { return 1.85 - A + 7.0 * pow(abs(A - 0.8), 3.0); }   // Burley Eq.5, per channel

// Front scatter: soften + tint the terminator. The per-channel wrapped cosine adds light beyond the hard
// NdotL horizon; the SSS term is only that EXTRA, tinted by subsurfaceColor (0 in the fully-lit region).
float3 SssWrapDiffuse(float3 L, SurfaceBRDF s)
{
    float  NdotL = dot(s.N, L);
    float3 w  = saturate(kSssWrap * s.scatterRadius) * (s.subsurfaceColor / max(SssProfileShape(s.subsurfaceColor), 1.0e-3));
    float3 Cw = saturate((NdotL + w) / (1.0 + w));
    return s.subsurfaceColor * max(Cw - saturate(NdotL), float3(0.0)) / kPi;
}

// Back scatter (Frostbite / Barre-Brisebois): light transmitted through thin regions toward the viewer.
// Thin (low thickness) glows more; carries its own view geometry (no NdotL factor).
float3 SssTranslucency(float3 L, float3 V, SurfaceBRDF s)
{
    float3 Ht   = normalize(L + s.N * kSssDistort);
    float  back = pow(saturate(dot(V, -Ht)), kSssPower);
    return s.subsurfaceColor * (back * (1.0 - saturate(s.thickness)));
}

// Chiang 2016 reparameterization for the PT random walk: surface albedo A -> single-scatter albedo alpha +
// the extinction from the authored mfp. sigma_s = alpha*sigma_t, sigma_a = (1-alpha)*sigma_t (per channel).
public struct SssSigma { public float3 sigmaS; public float3 sigmaA; }
public SssSigma SssAlbedoToSigma(float3 A, float radius)
{
    float3 alpha  = 1.0 - exp((-5.09406 + (2.61188 - 4.31805 * A) * A) * A);   // Horner of Chiang's cubic fit
    float3 d      = 1.9 - alpha + 3.5 * (alpha - 0.8) * (alpha - 0.8);         // diffuse-transmission scaling
    float3 sigmaT = 1.0 / max(radius * d, 1.0e-4);
    SssSigma o;
    o.sigmaS = alpha * sigmaT;
    o.sigmaA = (1.0 - alpha) * sigmaT;
    return o;
}

// Layered evaluate-at-surface-point: aniso base + clear-coat + sheen (NdotL > 0), plus the subsurface
// wrap/translucency which have support below the horizon. see arch/rendering-pipeline.md
public float3 EvalSurfBRDFTimesNdotL(float3 L, float3 radiance, float3 V, SurfaceBRDF s)
{
    bool  anisoActive = abs(s.anisotropy) > 1.0e-3;
    bool  glassActive = s.transmission > 0.0;
    float sheenW      = max(s.sheenColor.r, max(s.sheenColor.g, s.sheenColor.b));
    float sssW        = max(s.subsurfaceColor.r, max(s.subsurfaceColor.g, s.subsurfaceColor.b));
    bool  sssActive   = sssW > 0.0;
    if (!anisoActive && s.clearcoat <= 0.0 && !glassActive && sheenW <= 0.0 && !sssActive)
        return EvalBRDFTimesNdotL(L, radiance, V, s.N, s.albedo, s.metallic, s.roughness);

    float NdotL = dot(s.N, L);
    // Below the horizon only subsurface contributes (wrap past the terminator + back-lit translucency); the
    // reflection lobes have no sub-horizon support.
    if (NdotL <= 0.0)
        return sssActive ? (SssWrapDiffuse(L, s) + SssTranslucency(L, V, s)) * radiance : float3(0.0);
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

    // Sheen: Charlie retro-lobe under the coat; the base loses the lobe's directional albedo (Filament
    // sheenScaling, view-side comp). No Fresnel by construction -- sheenColor is the reflectance.
    // invariant: sheenColor == 0 never enters this block, so a sheen-free material is bit-identical.
    if (sheenW > 0.0)
    {
        float  sr = clamp(s.sheenRoughness, 0.04, 1.0);
        float3 fSheen = s.sheenColor * (D_Charlie(sr, NoH) * V_Charlie(sr, NoV, NoL));
        base = base * (1.0 - sheenW * E_Sheen(NoV, sr)) + fSheen;
    }

    if (s.clearcoat > 0.0)
    {
        float ccR  = clamp(s.clearcoatRoughness, 0.04, 1.0);
        float Fc   = s.clearcoat * (0.04 + 0.96 * pow(1.0 - HoV, 5.0));    // F0=0.04 (IOR 1.5), weight-scaled
        float coat = D_GGX(s.N, H, ccR) * V_SmithGGXCorrelated(NoV, NoL, ccR * ccR) * Fc;
        base = base * (1.0 - Fc) + float3(coat);
    }

    // Subsurface (NdotL > 0 side): additive local diffusion approx -- wrapped/tinted terminator + thickness
    // translucency, carrying their own geometry (not the outer NdotL). invariant: subsurfaceColor == 0 ->
    // sssActive false -> sssTerms 0 -> bit-identical to base * radiance * NdotL. The true non-local transport
    // is the path_trace random-walk (Option A). see arch/rendering-pipeline.md
    float3 sssTerms = sssActive ? (SssWrapDiffuse(L, s) + SssTranslucency(L, V, s)) : float3(0.0);
    return (base * NdotL + sssTerms) * radiance;
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
    // Achromatic clear interface: the transmission tint is the VOLUME absorption (attenuationColor, applied
    // as Beer-Lambert in the PT medium / raster beer term), NOT baseColor -- keeps raster == RT and lets a
    // clear glass stay clear regardless of its albedo.
    return float3(tw * (1.0 - F) * ft);
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
