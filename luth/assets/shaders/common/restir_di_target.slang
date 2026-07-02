// ReSTIR DI combined diffuse+spec RIS target, DI-shader-only. Twin of restir_di_target.glsl.
// GI_PI is internal here (the GI path defines its own); only RestirTargetPdf is exported.
// see arch/rendering-pipeline.md
import restir_common;   // Luminance
import brdf;            // D_GGX / G_Smith

static const float GI_PI = 3.14159265359;

// pHat = Luminance(Li)*(NoL + pi*D*G/(4*NoV)), F0-/albedo-free, RIS-importance-samples BOTH lobes. The pi
// balances the diffuse term's dropped 1/pi; reduces to the diffuse target when the spec lobe -> 0. Unbiased.
public float RestirTargetPdf(float3 N, float3 V, float3 L, float3 Li, float roughness)
{
    float  NoL  = max(dot(N, L), 0.0);
    float3 H    = normalize(V + L);
    float  NoV  = max(dot(N, V), 1.0e-4);
    float  spec = GI_PI * D_GGX(N, H, roughness) * G_Smith(N, V, L, roughness) / (4.0 * NoV);
    return Luminance(Li) * (NoL + spec);
}
