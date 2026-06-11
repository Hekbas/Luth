#version 450
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : enable

#include "common/globals.glsl"
#include "common/pbr_surface.glsl"
#include "common/pbr_lighting.glsl"

layout(location = 0) in vec3 v_WorldPos;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec2 v_TexCoord0;
layout(location = 6) in vec2 v_TexCoord1;
layout(location = 3) in mat3 v_TBN;    // locations 3, 4, 5
layout(location = 7) flat in uint v_MaterialIndex;
layout(location = 8) flat in uint v_ShadeMode;
layout(location = 9) flat in uint v_EntityID;

layout(location = 0) out vec4 outColor;
layout(location = 1) out uint outEntityID;

// ---------- Opaque-only descriptor bindings ----------
// Everything below is screen-space state computed from the OPAQUE depth prepass — correct here,
// wrong for any surface at a different depth. The transparent-pass shaders consume only the
// pbr_surface/pbr_lighting seams and never declare these.

// Set 0: GTAO final AO buffer (half-res R8) + settings
layout(set = 0, binding = 4) uniform sampler2D   gtaoTex;
layout(set = 0, binding = 5, std140) uniform GTAOUBO {
    float intensity;
    float radius;
    float falloff;
    float power;
    int   sliceCount;
    int   stepsPerSlice;
    int   enabled;
    int   visualize;
    vec2  invResolution;
    vec2  invFullResolution;
} gtao;

// RT-traced sun-shadow mask — written by rt_sun_shadows.comp when ShadowingMode::RtShadows is
// active. Sampled at screen UV (gl_FragCoord.xy / viewportSize). Single R8 channel = visibility
// (0 = in shadow, 1 = unshadowed). Reads only happen on the RT branch of ComputeShadow below; the
// CSM branch never dynamically accesses this binding so layout/initialization quirks of the mask
// (e.g., CSM-mode-only frames) don't affect the cascade path.
layout(set = 3, binding = 4) uniform sampler2D sunShadowMask;

// CONTRACT: diIrradiance stores DEMODULATED diffuse irradiance E = Li * NdotL * W (no albedo,
// no 1/PI) — see restir_shade.comp. Remodulated here as E * (albedo * (1-metallic) / PI), sampled
// only when restirParams.x > 0.5; else the per-cluster point-light loop runs.
layout(set = 3, binding = 5) uniform sampler2D diIrradiance;

// ReSTIR GI demodulated indirect-diffuse irradiance — same contract as diIrradiance. ADDED to Lo
// (alongside DI, not instead of) when restirParams.y > 0.5; remodulated identically.
layout(set = 3, binding = 6) uniform sampler2D giIrradiance;

// CONTRACT: reflRadiance stores the DEMODULATED reflected radiance (rt_reflections.comp). Composited below
// as a DIRECT specular reflection that supersedes the prefiltered-env IBL specular above the roughness
// cutoff — added at full strength (×ao, ×reflWeight), NOT scaled by iblIntensity (it's a traced scene
// reflection, not an IBL approximation). The denoiser owns the slot; reflParams.x gates the consumption.
layout(set = 3, binding = 7) uniform sampler2D reflRadiance;

// RT sun-shadow visibility — samples the per-view R8 mask at the current pixel's screen UV.
// Returns cascade=-1 since RT mode has no cascade selection (debug viz reuses the flag for an
// RT-specific overlay below).
ShadowResult ComputeShadowRT(vec2 uv)
{
    if (ubo.shadowBias.x < 0.0)
        return ShadowResult(1.0, -1);
    return ShadowResult(texture(sunShadowMask, uv).r, -1);
}

// Top-level dispatcher. rtShadowParams.x = 0 → cascade PCF (CSM mode, bit-identical to v3.0.9);
// rtShadowParams.x > 0.5 → ray-traced mask sample.
ShadowResult ComputeShadow(vec3 worldPos, vec3 N, vec2 uv)
{
    if (ubo.rtShadowParams.x > 0.5)
        return ComputeShadowRT(uv);
    return ComputeShadowCSM(worldPos, N);
}

// ---------- Main ----------

void main()
{
    GPUMaterialData mat = materials[v_MaterialIndex];
    PbrSurface s = EvalPbrSurface(mat, v_TexCoord0, v_TexCoord1, v_TBN, v_Normal, gl_FrontFacing);

    vec4  albedo    = s.albedo;
    vec3  emission  = s.emission;
    vec3  N         = s.N;
    float metallic  = s.metallic;
    float roughness = s.roughness;

    // Tokuyoshi 2019 "Improved Geometric Specular Antialiasing" (HPG). Screen-space normal curvature
    // adds variance to BRDF roughness — kills high-freq specular sparkle on curved surfaces at glancing
    // angles. The σ² term is the projected normal variance over a pixel footprint; we add it to α² (=
    // roughness²) and take sqrt back. Disabled → roughness passes through unmodified.
    if (ubo.specAaParams.x != 0.0)
    {
        vec3  dNdu     = dFdx(N);
        vec3  dNdv     = dFdy(N);
        float sigma2   = ubo.specAaParams.y * ubo.specAaParams.y;
        float variance = sigma2 * (dot(dNdu, dNdu) + dot(dNdv, dNdv));
        roughness      = clamp(sqrt(roughness * roughness + variance), 0.04, 1.0);
    }

    // --- Always write entity ID for picking ---
    outEntityID = v_EntityID;

    // --- Shade mode overrides ---
    if (v_ShadeMode == 1u) { outColor = vec4(albedo.rgb, 1.0); return; }  // Unlit
    if (v_ShadeMode == 3u) { outColor = vec4(N * 0.5 + 0.5, 1.0); return; }  // Normals
    if (v_ShadeMode == 4u) {  // EntityID debug visualization
        float id = float(v_EntityID);
        vec3 idColor = vec3(fract(id * 0.123), fract(id * 0.456), fract(id * 0.789));
        outColor = vec4(idColor, 1.0);
        return;
    }
    if (v_ShadeMode == 13u) { outColor = vec4(emission, 1.0); return; }  // Emission (ShadeMode::Emission)

    // --- Ambient Occlusion ---
    // Material-baked AO * screen-space GTAO. GTAO term is fetched
    // from a half-res R8 buffer via bilinear sampling; when the pass is
    // disabled at runtime, gtao.enabled == 0 and we skip the texture read.
    float ao = s.materialAO;
    if (gtao.enabled != 0)
    {
        vec2 gtaoUV = gl_FragCoord.xy * gtao.invFullResolution;
        float gtaoAO = texture(gtaoTex, gtaoUV).r;
        ao *= gtaoAO;
    }

    // --- Lighting ---
    vec3 V = normalize(ubo.cameraPos - v_WorldPos);
    vec3 Lo = vec3(0.0);

    // Directional light + shadow (cascade PCF in CSM mode, R8 mask sample in RT mode).
    vec2 shadowUv = gl_FragCoord.xy / ubo.viewportSize;
    ShadowResult sr = ComputeShadow(v_WorldPos, N, shadowUv);

    {
        vec3 dirRadiance = lights.dirLight.color * lights.dirLight.intensity;
        Lo += EvalBRDFTimesNdotL(normalize(-lights.dirLight.direction), dirRadiance, V, N, albedo.rgb, metallic, roughness) * sr.shadow;
    }

    // Point lights. restirParams.x > 0.5 → sample the demodulated DI image (remodulate by diffuse-
    // albedo/PI, CONTRACT above); else the unshadowed Forward+ cluster loop.
    if (ubo.restirParams.x > 0.5)
    {
        // Diffuse albedo = baseColor * (1 - metallic); metals carry no Lambertian diffuse.
        Lo += texture(diIrradiance, gl_FragCoord.xy / ubo.viewportSize).rgb * (albedo.rgb * (1.0 - metallic) / GI_PI);
    }
    else
    {
        Lo += EvalClusteredPointLights(gl_FragCoord, v_WorldPos, V, N, albedo.rgb, metallic, roughness);
    }

    // ReSTIR GI — demodulated indirect diffuse, ADDED on top of the direct term (independent of the
    // DI gate). Same remodulation as DI: E * (diffuse-albedo / PI).
    if (ubo.restirParams.y > 0.5)
    {
        Lo += texture(giIrradiance, gl_FragCoord.xy / ubo.viewportSize).rgb * (albedo.rgb * (1.0 - metallic) / GI_PI);
    }

    // IBL ambient lighting
    IblResult ibl = EvalIblAmbient(N, V, albedo.rgb, metallic, roughness, ao, ubo.iblIntensity);
    vec3 color = ibl.ambient + Lo;

    // RT specular reflection (D.1) — reflRad is the actual traced reflected radiance (scene-lit by its own
    // NEE + occluded by the ray), so it is DECOUPLED from iblIntensity (the IBL artistic knob): point-lit
    // surroundings reflect at full strength even when the env intensity is low. It SUPERSEDES the
    // prefiltered-env IBL specular above the roughness cutoff — add reflRad·envBRDF and subtract the
    // iblSpecular it replaces, both ×ao (contact) ×reflWeight (roughness fade). reflRad's env-on-miss
    // already tracks iblIntensity, so sky reflections stay consistent across the RT↔IBL fade.
    if (ubo.reflParams.x > 0.5)
    {
        float reflWeight = 1.0 - smoothstep(ubo.reflParams.y, ubo.reflParams.z, roughness);
        vec3  reflRad    = texture(reflRadiance, gl_FragCoord.xy / ubo.viewportSize).rgb;
        color += (reflRad * ibl.envBRDF - ibl.specularIBL * ubo.iblIntensity) * ao * reflWeight;
    }

    // Debug viz: replace final color with the raw AO buffer so the user can
    // see the GTAO result in isolation (togglable from the Render panel).
    if (gtao.visualize != 0)
    {
        vec2 gtaoUV = gl_FragCoord.xy * gtao.invFullResolution;
        float gtaoAO = texture(gtaoTex, gtaoUV).r;
        outColor = vec4(vec3(gtaoAO), 1.0);
        outEntityID = v_EntityID;
        return;
    }

    // Debug visualization for sun shadows. In CSM mode tints each cascade; in RT mode replaces
    // final color with the raw R8 mask as grayscale so you can see what the raygen wrote.
    if (ubo.debugVisualizeCascades > 0.5)
    {
        if (ubo.rtShadowParams.x > 0.5)
        {
            float vis = texture(sunShadowMask, shadowUv).r;
            outColor = vec4(vis, vis, vis, 1.0);
            outEntityID = v_EntityID;
            return;
        }
        else if (sr.cascade >= 0)
        {
            const vec3 cascadeColors[4] = vec3[4](
                vec3(1.0, 0.2, 0.2),   // cascade 0 — red (near)
                vec3(0.2, 1.0, 0.2),   // cascade 1 — green
                vec3(0.2, 0.2, 1.0),   // cascade 2 — blue
                vec3(1.0, 1.0, 0.2)    // cascade 3 — yellow (far)
            );
            color = mix(color, cascadeColors[sr.cascade], 0.25);
        }
    }

    outColor = vec4(color + emission, albedo.a);
}
