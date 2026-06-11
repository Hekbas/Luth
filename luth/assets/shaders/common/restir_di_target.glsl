// ReSTIR DI combined diffuse+spec RIS target (#154). DI-shader-only — kept OUT of the shared
// restir_common.glsl because the GI path (restir_gi_common.glsl) includes that header and declares its
// own `const float GI_PI`; a GI_PI macro there would poison the const. This file pulls brdf + GI_PI only
// into the DI shaders (initial/temporal/spatial/shade). see arch/rendering-pipeline.md
#ifndef LUTH_SHADERS_RESTIR_DI_TARGET
#define LUTH_SHADERS_RESTIR_DI_TARGET

#ifndef GI_PI
#define GI_PI 3.14159265359
#endif
#include "common/restir_common.glsl"   // Luminance
#include "common/brdf.glsl"             // D_GGX / G_Smith

// pHat = Luminance(Li)·(NoL + π·D·G/(4·NoV)) — F0-/albedo-free, proportional to the integrand so RIS
// importance-samples BOTH lobes (canonical ReSTIR target = full BSDF). The π balances the diffuse term's
// dropped 1/π, so it reduces to the old diffuse target when the spec lobe → 0. Still unbiased.
float RestirTargetPdf(vec3 N, vec3 V, vec3 L, vec3 Li, float roughness) {
    float NoL  = max(dot(N, L), 0.0);
    vec3  H    = normalize(V + L);
    float NoV  = max(dot(N, V), 1.0e-4);
    float spec = GI_PI * D_GGX(N, H, roughness) * G_Smith(N, V, L, roughness) / (4.0 * NoV);
    return Luminance(Li) * (NoL + spec);
}

#endif
