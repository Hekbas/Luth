#pragma once

#include "luth/renderer/settings/GTAOSettings.h"

namespace Luth
{
    enum class TonemapOperator : int {
        Linear     = 0,
        Reinhard   = 1,
        ACES       = 2,
        Uncharted2 = 3,
        AgX        = 4,
        AgXPunchy  = 5
    };

    struct PostProcessSettings
    {
        // Bloom
        float bloomThreshold  = 1.0f;
        float bloomStrength   = 0.5f;
        float bloomRadius     = 1.0f;  // scatter: upsample tent spread (wider = softer, broader halo)

        // Tone mapping
        TonemapOperator tonemapOp = TonemapOperator::ACES;
        float exposure   = 1.0f;
        float contrast   = 1.0f;
        float saturation = 1.0f;

        // Color balance
        Vec3 shadowBalance    = Vec3(1.0f);
        Vec3 midtoneBalance   = Vec3(1.0f);
        Vec3 highlightBalance = Vec3(1.0f);

        // Effects
        float vignetteAmount      = 0.0f;
        float vignetteHardness    = 0.5f;
        float grainAmount         = 0.0f;
        float sharpness           = 0.0f;
        float chromaticAberration = 0.0f;

        // Specular antialiasing (Tokuyoshi 2019). Default-on; at sigma 0.5 it's a no-op on flat
        // surfaces and only kicks in where screen-space normal curvature would alias the BRDF.
        bool  specularAaEnabled = true;
        float specularAaSigma   = 0.5f;

        // Temporal antialiasing (Karis 2014 YCoCg-clip recipe). taaTemporalAlpha is the
        // current-frame feedback weight (0.1 = 90% history, Karis default).
        bool  taaEnabled       = true;
        float taaTemporalAlpha = 0.1f;

        // Ambient occlusion (GTAO)
        GTAOSettings gtao;
    };

    // GPU-side UBO layout (std140 compatible, matches postprocess.frag)
    struct PostProcessUBO
    {
        float bloomThreshold;       // 0
        float bloomStrength;        // 4
        float exposure;             // 8
        float contrast;             // 12

        float saturation;           // 16
        int   tonemapOp;            // 20
        float vignetteAmount;       // 24
        float vignetteHardness;     // 28

        float grainAmount;          // 32
        float sharpness;            // 36
        float chromaticAberration;  // 40
        float time;                 // 44

        Vec3 shadowBalance;    float _pad0; // 48-60
        Vec3 midtoneBalance;   float _pad1; // 64-76
        Vec3 highlightBalance; float _pad2; // 80-92
        // Total: 96 bytes
    };
}
