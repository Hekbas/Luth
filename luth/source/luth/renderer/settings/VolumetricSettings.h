#pragma once

#include "luth/core/types/LuthMath.h"

namespace Luth
{
    // CPU authoring values for global volumetric parameters. Mirrored each frame into the Global UBO
    // (distance fog + height fog + multi-scatter scalar) so the composite + inject shaders can read
    // them without a separate binding. Persisted alongside PostProcessSettings on the project.
    struct VolumetricSettings
    {
        // Distance fog — exponential extinction with camera-to-fragment distance.
        bool distanceFogEnabled    = true;
        f32  distanceFogDensity    = 0.01f;
        f32  distanceFogStart      = 0.0f;
        f32  distanceFogMaxOpacity = 0.85f;
        Vec3 distanceFogColor      = Vec3(0.5f, 0.6f, 0.7f);

        // Height fog — exponential extinction with world-space Y vs reference height.
        bool heightFogEnabled   = false;
        f32  heightFogDensity   = 0.05f;
        f32  heightFogRefHeight = 0.0f;
        f32  heightFogFalloff   = 0.1f;
        Vec3 heightFogColor     = Vec3(0.4f, 0.5f, 0.6f);

        // Henyey-Greenstein phase anisotropy. 0 = isotropic; positive = forward scatter (god rays);
        // negative = backscatter. Typical fog: 0.3-0.7.
        f32  anisotropy = 0.4f;

        // 2nd-order multi-scatter — adds IBL ambient term modulated by extinction, the proper
        // Wronski/Frostbite approximation. 0 = disabled. Replaces the misnamed v3.0.4 "Hillaire"
        // multiplicative boost which couldn't lift shadowed regions.
        f32  multiScatterIntensity = 0.0f;

        // Temporal accumulation blend (resolve pass). Larger = less smoothing, more responsive;
        // smaller = more smoothing, more ghosting. Wronski recommends 0.05.
        f32  temporalAlpha = 0.05f;

        // Number of shadow-ray steps from voxel toward sun, accumulating fog optical depth along
        // the light path (proper sun-self-shadowing in dense fog). 0 = disabled; 4 = quality.
        i32  sunFogAbsorptionSteps = 4;

        // Multiplier on volumetric fog opacity at sky pixels. 1.0 = full fog on sky (skybox can be
        // entirely hidden in dense fog); 0.0 = sky bypasses volumetric fog. Decoupled from the
        // analytic distance fog max opacity.
        f32  skyFogStrength = 1.0f;

        // Viz pass tunables — multiplier on sampled value + overlay alpha. ScaleDensity tunes the
        // density heat-map's bright range; scaleInScatter tunes the radiance overlay; opacity sets
        // how much the underlying scene shows through.
        f32  vizScaleDensity   = 5.0f;
        f32  vizScaleInScatter = 0.5f;
        f32  vizOpacity        = 0.75f;

        // 3D Worley-FBM noise modulating fog density per voxel (Larian/BG3 style wispy look). Scale
        // is world-space frequency (units: 1/m); larger = smaller, more frequent clumps. Strength
        // is the per-voxel modulation amplitude [0..1]: 0 disables, 1.0 swings density 0→2× around
        // its mean. Wind animates the sample UV through time for a slow drift.
        f32  noiseScale    = 0.04f;
        f32  noiseStrength = 0.6f;
        Vec3 noiseWind     = Vec3(0.5f, 0.0f, 0.2f);

        // Atlas resolution preset. Changing recreates the per-view atlases + descriptors.
        enum class Quality : u32 { Low = 0, Medium = 1, High = 2 };
        Quality quality = Quality::Medium;
    };
}

namespace Luth::Volumetric
{
    // Resolves the 3D atlas dimensions for a given quality preset. Used by ViewResources for the
    // atlas allocations and by the subsystem for dispatch dims.
    struct AtlasDims { u32 x, y, z; };
    inline AtlasDims GetAtlasDims(VolumetricSettings::Quality q)
    {
        switch (q) {
            case VolumetricSettings::Quality::Low:    return { 80,  45, 64  };
            case VolumetricSettings::Quality::High:   return { 240, 135, 192 };
            case VolumetricSettings::Quality::Medium:
            default:                                  return { 160, 90, 128 };
        }
    }
}
