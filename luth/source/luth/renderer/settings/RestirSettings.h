#pragma once

#include "luth/core/types/LuthTypes.h"

namespace Luth
{
    // ReSTIR DI (Bitterli 2020) runtime-tunable parameters. Read each frame by RtRestirSubsystem
    // when filling the initial/temporal/spatial push constants; `enabled` also gates restirParams.x
    // in GlobalSubsystem (pbr.frag samples the demodulated DI image only when set). Persisted
    // alongside the other render settings on the RenderingSystem. see arch/rendering-pipeline.md
    struct RestirSettings
    {
        bool enabled = true;
        bool halfResolution = false;     // trace + denoise DI (diffuse + specular) half-res + bilateral upscale
        u32  candidateCount = 32;        // initial RIS candidates (M)
        u32  temporalMCap = 20;          // history clamp multiplier (prev.M <= mCap * curr.M)
        f32  temporalDepthThreshold = 0.05f;
        f32  temporalNormalThreshold = 0.9f;
        u32  spatialNeighbours = 5;
        u32  spatialRadius = 16;
        f32  spatialDepthThreshold = 0.1f;
        f32  spatialNormalThreshold = 0.9f; // min dot(neighbourN, currN); was a hardcoded shader constant
        f32  roughnessThreshold = 0.25f;    // spatial spec reuse gate: max |neighbourRough - rough|
        f32  boilingStrength = 0.2f;        // spatial-tail boiling filter: kill W above ~(10/s - 9)x the group avg; 0 = off
        bool specular = true;            // demodulated specular DI (metals/specular from point lights)
        f32  specularIntensity = 1.0f;   // composite scale, baked into restirParams.z (pbr.frag remod)
        f32  diSpecClamp = 64.0f;        // luminance cap on the demodulated spec lobe (grazing 1/(4*NoV) spike)
        f32  confidenceNorm = 512.0f;    // reservoir M mapping to full SVGF confidence (shade alpha = saturate(M/norm))
    };
}
